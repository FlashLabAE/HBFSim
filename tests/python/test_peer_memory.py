"""Native Peer residency, idle admission, and asynchronous slot ownership."""
from contextlib import contextmanager
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from hbfsim_client.simulation_session import ResolvedSystemConfig, SimulationSession
from hbfsim_client.transaction_protocol import Transaction, TransactionBatch

BINARY = Path(os.environ.get('E3_NATIVE_SIMULATOR', ROOT / 'build/hbfsim'))
PAGE = 4096


class PeerMemoryTests(unittest.TestCase):
    @contextmanager
    def session(self, *, policy='kv_first', slots=3, weights=1, initialized=(), **settings):
        with tempfile.TemporaryDirectory(prefix='peer-memory-') as directory:
            path = Path(directory) / 'policy.cfg'
            values = {'hbf-peer-cold-ns': 1000, 'hbf-peer-high-watermark': .9,
                'hbf-peer-low-watermark': .8, 'hbf-initial-free-blocks-erased': 'true',
                'hbf-write-coalescing': 'false', 'hbf-program-ns': 20000,
                'hbf-read-ns': 1000, **settings}
            path.write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
            system = ResolvedSystemConfig.load((ROOT/'tests/fixtures/simulation-session-mini.cfg', path)).resolve(BINARY)
            with SimulationSession(simulator_path=BINARY, system_config=system,
                                   enable_hbm=True, enable_hbf=True) as simulator:
                ranges = ([(0, weights*PAGE)] if weights else []) + list(initialized)
                initial = simulator.configure_peer_memory('peer-setup', policy=policy,
                    hbm_bytes=slots*PAGE, logical_bytes=64*PAGE, kv_begin=weights*PAGE,
                    unit_bytes=PAGE, transfer_bytes=PAGE, initialized=ranges)
                yield simulator, initial['peer_memory']

    @staticmethod
    def writes(simulator, batch, pages):
        return simulator.submit(TransactionBatch(batch, tuple(
            Transaction(f'b{batch}-w{page}', 'PEER', 'W', page*PAGE, PAGE, 0) for page in pages)))

    @staticmethod
    def idle(simulator, batch, duration):
        # No workload compute-window declaration: a native timer is sufficient.
        return simulator.submit(TransactionBatch(batch, (
            Transaction(f'b{batch}-idle', 'BARRIER', None, 0, 0, 0, duration_ns=duration),)))

    def test_both_priorities_fill_spare_hbm_with_existing_data(self):
        for policy, weight_pages, kv_pages in [('weights_first', 2, 1), ('kv_first', 1, 2)]:
            with self.subTest(policy=policy), self.session(policy=policy, weights=2,
                    initialized=[(2*PAGE, 4*PAGE)]) as (_, state):
                self.assertEqual(state['hbm_units'], 3)
                self.assertEqual(state['hbm_weight_bytes'], weight_pages*PAGE)
                self.assertEqual(state['hbm_kv_bytes'], kv_pages*PAGE)
                self.assertEqual(state['hbm_units']+state['hbf_units'], 4)

    def test_new_kv_replaces_opportunistic_weights_and_uses_hbm_first(self):
        with self.session(weights=3, **{'hbf-peer-writeback': 'false'}) as (sim, initial):
            self.assertEqual(initial['hbm_weight_bytes'], 3*PAGE)
            result = self.writes(sim, 0, [3, 4])
            state = result['peer_memory']
            self.assertEqual(state['forced_demotions'], 2)
            self.assertEqual(state['forced_bytes'], 2*PAGE)
            self.assertEqual(state['hbm_weight_bytes'], PAGE)
            self.assertEqual(state['hbm_kv_bytes'], 2*PAGE)
            self.assertEqual(state['kv_hbm_write_bytes'], 2*PAGE)
            self.assertEqual(state['kv_hbf_write_bytes'], 0)
            self.assertEqual(state['live_bytes'], 5*PAGE)

    def test_idle_writeback_uses_access_recency_and_preserves_foreground_time(self):
        with self.session(policy='weights_first') as (sim, _):
            self.writes(sim, 0, [1, 2])
            sim.submit(TransactionBatch(1, (Transaction('hot', 'PEER', 'R', PAGE, PAGE, 0),)))
            result = self.idle(sim, 2, 100000)
            self.assertEqual(result['elapsed_ns'], 100000)
            state = result['peer_memory']
            self.assertEqual(state['background_demotions'], 1)
            self.assertEqual(state['last_background_address'], 2*PAGE)
            self.assertEqual(state['hbm_kv_bytes'], PAGE)
            self.assertEqual(state['hbm_weight_bytes'], PAGE)
            self.assertEqual(state['forced_demotions'], 0)

    def test_busy_hbf_postpones_background_admission(self):
        with self.session(initialized=[(PAGE,4*PAGE)],
                          **{'hbf-read-ns': 20000}) as (sim, _):
            result = sim.submit(TransactionBatch(0, (
                Transaction('weights', 'PEER', 'R', 0, PAGE, 0),
                Transaction('timer', 'BARRIER', None, 0, 0, 0,
                            duration_ns=100000, dependencies=('weights',))), observe=('weights',)))
            weight_finish = next(row['finish_ns'] for row in result['observed_timings'] if row['id']=='weights')
            self.assertGreaterEqual(result['peer_memory']['last_background_start_ns'], weight_finish)
            self.assertEqual(result['peer_memory']['background_demotions'], 1)

    def test_inflight_copy_crosses_batch_and_fences_slot_reuse(self):
        with self.session(weights=0, initialized=[(0,3*PAGE)]) as (sim, _):
            idle = self.idle(sim, 0, 2000)
            self.assertEqual(idle['elapsed_ns'], 2000)
            self.assertEqual(idle['peer_memory']['migrating_units'], 1)
            result = self.writes(sim, 1, [3])
            self.assertGreater(result['blocking_finish_ns'], 20000)
            self.assertEqual(result['peer_memory']['forced_demotions'], 0)
            self.assertLessEqual(result['peer_memory']['hbm_reserved_units'], 3)
            self.assertEqual(result['peer_memory']['kv_hbm_write_bytes'], PAGE)

    def test_release_while_copy_is_pending_does_not_resurrect_kv(self):
        with self.session(weights=0, initialized=[(0,3*PAGE)]) as (sim, _):
            self.idle(sim, 0, 2000)
            released = sim.release_peer_memory('release-old', address=0, bytes=PAGE)
            self.assertEqual(released['peer_memory']['live_bytes'], 2*PAGE)
            self.assertEqual(released['peer_memory']['released_bytes'], PAGE)
            written = self.writes(sim, 1, [0])
            self.assertEqual(written['peer_memory']['live_bytes'], 3*PAGE)
            read = sim.submit(TransactionBatch(2, (Transaction('read-new', 'PEER', 'R', 0, PAGE, 0),)))
            self.assertEqual(read['peer_memory']['kv_hbm_read_bytes'], PAGE)

    def test_disabled_background_keeps_capacity_fallback(self):
        with self.session(weights=0, initialized=[(0,3*PAGE)],
                          **{'hbf-peer-writeback': 'false'}) as (sim, _):
            result = self.idle(sim, 0, 100000)
            self.assertEqual(result['peer_memory']['background_demotions'], 0)
            self.assertEqual(result['peer_memory']['hbm_units'], 3)
            result = self.writes(sim, 1, [3])
            self.assertEqual(result['peer_memory']['forced_demotions'], 1)

    def test_sparse_kv_survives_background_copy_and_demand_promotion(self):
        # Two initialized fragments of one page share one resident unit;
        # migration must not materialize the unwritten gap as valid KV.
        ranges = [(0,128), (256,384), (PAGE,3*PAGE)]
        with self.session(weights=0, initialized=ranges,
                          **{'hbf-write-coalescing': 'true'}) as (sim, _):
            idle = self.idle(sim, 0, 100000)
            self.assertEqual(idle['peer_memory']['background_bytes'], 256)
            result = sim.submit(TransactionBatch(1, (
                Transaction('read-first', 'PEER', 'R', 0, 128, 0),
                Transaction('write-gap', 'PEER', 'W', 128, 128, 0, dependencies=('read-first',)),
                Transaction('read-joined', 'PEER', 'R', 0, 384, 0, dependencies=('write-gap',)),)))
            self.assertEqual(result['peer_memory']['promoted_bytes'], 256)
            self.assertEqual(result['peer_memory']['live_bytes'], 2*PAGE+384)
            sim.close()
            final = sim.source_receipt()['final_measurement']['peer_memory']
            self.assertEqual(final['migrating_units'], 0)
            self.assertEqual(final['live_bytes'], 2*PAGE+384)


if __name__ == '__main__':
    unittest.main()
