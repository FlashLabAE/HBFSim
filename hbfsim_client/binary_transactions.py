"""Fixed-size transaction fields plus explicit IDs/dependencies; no device model."""
from dataclasses import dataclass
import hashlib
import math
from pathlib import Path
import struct
from hbfsim_client.transaction_protocol import TRANSACTION_TARGETS,require_safe_identifier

MAGIC=b'HBFTX01\n'
HEADER=struct.Struct('<HBBIQQddII')
TARGETS=('HBM','HBF_LOGICAL','HBF_STATIC','HBF_PHYSICAL','D2D_HBF_TO_HBM','D2D_HBM_TO_HBF','EXTERNAL','HOST_DRAM','DIRECT_HBF_TO_EXTERNAL','DIRECT_EXTERNAL_TO_HBF','BARRIER')
SUPPORTED={'HBM','HBF_LOGICAL','HBF_STATIC','HBF_PHYSICAL','BARRIER'}

@dataclass(frozen=True)
class BinaryTransactionBatch:
    batch_id: int
    path: Path
    logical_trace_sha256: str
    transaction_trace_sha256: str
    metrics: dict
    frontier: tuple[str,...]
    retain: tuple[str,...]
    completions: bool=False
    required_capability: str="binary_transactions_v1"
    stream_window_records: int=1024
    @property
    def frontier_transactions(self):return len(self.frontier)
    def begin_line(self):
        command = 'BEGIN_STREAM' if self.required_capability == 'serial_request_stream_v1' else 'BEGIN_BINARY'
        return f'{command} {self.batch_id} {self.logical_trace_sha256} {self.transaction_trace_sha256} frontier={",".join(self.frontier) or "-"} retain={",".join(self.retain) or "-"} completions=0'
    def protocol_payload_chunks(self):
        suffix = f' {self.stream_window_records}' if self.required_capability == 'serial_request_stream_v1' else ''
        yield f'FILE {str(self.path.resolve()).encode().hex()} 0 {self.path.stat().st_size}{suffix}\n'

class BinaryBatchWriter:
    def __init__(self,path,*,burst_bytes=32,page_bytes=4096):
        self.path=Path(path);self.stream=self.path.open('xb');self.stream.write(MAGIC+bytes(8))
        self.burst_bytes=burst_bytes;self.page_bytes=page_bytes
        self.metrics={'census':{t:{'transactions':0,'bytes':0} for t in TRANSACTION_TARGETS},
          'latency':{t:{'read':0,'write':0} for t in TRANSACTION_TARGETS},
          'transactions':0,'memory_transactions':0,'barriers':0,'dependency_edges':0,'transaction_bytes':0,
          'hbm_requests':0,'hbm_bursts':0,'hbf_reads':0,'hbf_read_pages':0}
    def add(self,id,target,op,addr,size,issue=0.,duration=0.,dependencies=(),dependency_count=None):
        name=require_safe_identifier(id,'binary transaction id').encode('ascii')
        if not 0<len(name)<=4096 or target not in SUPPORTED:raise ValueError('unsupported binary ID/target')
        if target=='BARRIER':
            if op is not None or addr or size:raise ValueError('invalid binary barrier')
        elif op not in {'R','W'} or duration or type(size)is not int or size<=0:raise ValueError('invalid binary memory fields')
        if type(addr)is not int or addr<0 or addr+size>=2**64 or not all(math.isfinite(v) and v>=0 for v in (issue,duration)):raise ValueError('invalid binary address/time')
        if dependency_count is None:dependency_count=len(dependencies)
        if not 0<=dependency_count<=2000000:raise ValueError('binary dependency cap')
        self.stream.write(HEADER.pack(len(name),TARGETS.index(target),2 if target=='BARRIER' else int(op=='W'),0,addr,size,issue,duration,dependency_count,0));self.stream.write(name)
        actual=0
        for dep in dependencies:
            body=require_safe_identifier(dep,'binary dependency').encode('ascii')
            if not 0<len(body)<=4096:raise ValueError('binary dependency ID size')
            self.stream.write(struct.pack('<H',len(body))+body);actual+=1
        if actual!=dependency_count:raise ValueError('binary dependency count mismatch')
        m=self.metrics;m['transactions']+=1;m['dependency_edges']+=actual;m['transaction_bytes']+=size
        m['census'][target]['transactions']+=1;m['census'][target]['bytes']+=size
        if target=='BARRIER':m['barriers']+=1
        else:
            m['memory_transactions']+=1;m['latency'][target]['read' if op=='R' else 'write']+=1
            if target=='HBM':m['hbm_requests']+=1;m['hbm_bursts']+=(addr%self.burst_bytes+size+self.burst_bytes-1)//self.burst_bytes
            elif op=='R':m['hbf_reads']+=1;m['hbf_read_pages']+=(addr%self.page_bytes+size+self.page_bytes-1)//self.page_bytes
    def finish(self,*,batch_id,logical_sha256,frontier,retain):
        self.stream.seek(8);self.stream.write(struct.pack('<Q',self.metrics['transactions']));self.stream.close()
        if self.path.stat().st_size>512*1024**2 or self.metrics['transactions']>2000000:raise ValueError('binary batch exceeds cap')
        for value in (logical_sha256,):
            if len(value)!=64 or any(c not in '0123456789abcdef' for c in value):raise ValueError('invalid logical SHA')
        for name in (*frontier,*retain):require_safe_identifier(name,'binary frontier/retain')
        digest=hashlib.sha256()
        with self.path.open('rb') as f:
            while body:=f.read(1024*1024):digest.update(body)
        return BinaryTransactionBatch(batch_id,self.path.resolve(),logical_sha256,digest.hexdigest(),self.metrics,tuple(frontier),tuple(retain))


PHASE_MAGIC = b"HBFPH01\n"
PHASE_HEADER = struct.Struct("<8sQQQ")
PHASE_RECORD = struct.Struct("<BBHQQ")


class BinaryPhaseWriter:
    """Lossless shorthand for zero-issue memory requests plus a phase barrier.

    IDs, previous-phase edges and the all-request completion barrier are fixed
    by phase_index/first_request, and reconstructed by the native decoder.
    Arbitrary dependencies/timing belong to BinaryBatchWriter, not this format.
    """

    def __init__(self, path, *, phase_index, first_request, burst_bytes=32,
                 page_bytes=4096, stream_window=None):
        if stream_window is not None and (type(stream_window) is not int or not 1 <= stream_window <= 65536):
            raise ValueError("invalid stream window")
        self.stream_window = stream_window
        for value in (phase_index, first_request):
            if type(value) is not int or not 0 <= value < 2**64:
                raise ValueError("invalid binary phase index/range")
        if burst_bytes <= 0 or page_bytes <= 0:
            raise ValueError("invalid binary phase geometry")
        self.path = Path(path)
        self.stream = self.path.open("xb")
        self.stream.write(PHASE_HEADER.pack(PHASE_MAGIC, 0, phase_index, first_request))
        self.phase_index = phase_index
        self.first_request = first_request
        self.burst_bytes = burst_bytes
        self.page_bytes = page_bytes
        self.count = 0
        self.metrics = {
            "census": {t: {"transactions": 0, "bytes": 0} for t in TRANSACTION_TARGETS},
            "latency": {t: {"read": 0, "write": 0} for t in TRANSACTION_TARGETS},
            "transactions": 0, "memory_transactions": 0, "barriers": 1,
            "dependency_edges": 0, "transaction_bytes": 0,
            "hbm_requests": 0, "hbm_bursts": 0, "hbf_reads": 0, "hbf_read_pages": 0,
        }

    def add(self, target, op, addr, size):
        if target not in (TARGETS[:2] if self.stream_window else TARGETS[:4]) or op not in {"R", "W"}:
            raise ValueError("unsupported binary phase target/op")
        if (type(addr) is not int or type(size) is not int or addr < 0 or
                size <= 0 or addr + size >= 2**64):
            raise ValueError("invalid binary phase address/size")
        if self.count >= (25000000 if self.stream_window else 1999999) or self.first_request + self.count >= 2**64 - 1:
            raise ValueError("binary phase count/range exceeds cap")
        self.stream.write(PHASE_RECORD.pack(TARGETS.index(target), int(op == "W"),
                                           0, addr, size))
        self.count += 1
        m = self.metrics
        m["census"][target]["transactions"] += 1
        m["census"][target]["bytes"] += size
        m["latency"][target]["read" if op == "R" else "write"] += 1
        m["transaction_bytes"] += size
        if target == "HBM":
            m["hbm_requests"] += 1
            m["hbm_bursts"] += (addr % self.burst_bytes + size + self.burst_bytes - 1) // self.burst_bytes
        elif op == "R":
            m["hbf_reads"] += 1
            m["hbf_read_pages"] += (addr % self.page_bytes + size + self.page_bytes - 1) // self.page_bytes

    def finish(self, *, logical_sha256):
        if (len(logical_sha256) != 64 or
                any(c not in "0123456789abcdef" for c in logical_sha256)):
            raise ValueError("invalid binary phase logical SHA")
        self.stream.seek(8)
        self.stream.write(struct.pack("<Q", self.count))
        self.stream.close()
        m = self.metrics
        m["transactions"] = self.count + 1
        m["memory_transactions"] = self.count
        m["dependency_edges"] = self.count + (self.count + 1 if self.phase_index else 0)
        m["census"]["BARRIER"]["transactions"] = 1
        digest = hashlib.sha256()
        with self.path.open("rb") as stream:
            while body := stream.read(1024 * 1024):
                digest.update(body)
        final = f"replay/{self.phase_index}/complete"
        return BinaryTransactionBatch(
            self.phase_index, self.path.resolve(), logical_sha256,
            digest.hexdigest(), m, (final,), (final,),
            required_capability="serial_request_stream_v1" if self.stream_window else "binary_serial_phase_v1",
            stream_window_records=self.stream_window or 1024)

    def abort(self):
        """Close a failed partial file, retaining it for diagnosis."""
        self.stream.close()
