"""Bounded binary live kernel transport; failure is terminal, not rollback-safe."""
import hashlib
from types import SimpleNamespace
try:
    import numpy as np
except ImportError as error:
    raise RuntimeError('live replay requires NumPy') from error
from .transaction_protocol import TRANSACTION_TARGETS
from .simulation_session import SimulationSessionError

DTYPE=np.dtype([('target','u1'),('op','u1'),('reserved','<u2'),('addr','<u8'),('bytes','<u8')])

class LiveRequestStream:
    def __init__(self,session,*,phase,first,logical_sha256):
        if session._closed or not getattr(session,'_live_request_stream_v1',False):
            raise SimulationSessionError('backend lacks live_request_stream_v1')
        if session._enable_external or session._enable_host_dram or session._initial_hbf_persistent_artifact is not None:
            raise SimulationSessionError('unsupported live session')
        if type(phase) is not int or type(first) is not int or phase<0 or first<0 or phase>=2**64 or first>=2**64:
            raise ValueError('invalid live phase/range')
        if len(logical_sha256)!=64 or any(c not in '0123456789abcdef' for c in logical_sha256):
            raise ValueError('invalid live logical SHA')
        self.session=session;self.phase=phase;self.first=first;self.logical=logical_sha256
        self.digest=hashlib.sha256();self.count=0;self.finished=False
        self.metrics=dict(census={t:dict(transactions=0,bytes=0) for t in TRANSACTION_TARGETS},latency={t:dict(read=0,write=0) for t in TRANSACTION_TARGETS},transactions=0,memory_transactions=0,barriers=1,dependency_edges=0,transaction_bytes=0,hbm_requests=0,hbm_bursts=0,hbf_reads=0,hbf_read_pages=0)
        session._process.stdin.write(f'BEGIN_LIVE {phase} {first} {logical_sha256}\n');session._process.stdin.flush()
    def write(self,body):
        if self.finished or len(body)%20:raise ValueError('invalid live chunk')
        a=np.frombuffer(body,dtype=DTYPE);n=len(a)
        if not 1<=n<=65536 or self.count+n>25000000 or self.first+self.count+n>=2**64:
            raise ValueError('live chunk count/range exceeds cap')
        if np.any(a['target']>1) or np.any(a['op']>1) or np.any(a['reserved']) or np.any(a['bytes']==0) or np.any(a['addr']+a['bytes']<=a['addr']):
            raise ValueError('invalid live fields')
        m=self.metrics
        for code,target in enumerate(('HBM','HBF_LOGICAL')):
            mask=a['target']==code;v=a[mask];k=len(v)
            m['census'][target]['transactions']+=k;m['census'][target]['bytes']+=int(v['bytes'].sum())
            for op,label in ((0,'read'),(1,'write')):m['latency'][target][label]+=int(np.count_nonzero(v['op']==op))
            if code==0:
                burst=self.session._system_config.hbm_burst_bytes
                m['hbm_requests']+=k;m['hbm_bursts']+=int(((v['addr']%burst+v['bytes']+burst-1)//burst).sum())
            else:
                reads=v[v['op']==0];page=self.session._system_config.hbf_geometry.page_size_bytes
                m['hbf_reads']+=len(reads);m['hbf_read_pages']+=int(((reads['addr']%page+reads['bytes']+page-1)//page).sum())
        self.count+=n;m['transaction_bytes']+=int(a['bytes'].sum());self.digest.update(body)
        p=self.session._process.stdin;p.write(f'CHUNK {n}\n');p.flush();p.buffer.write(body);p.buffer.write(b'\n');p.buffer.flush()
    def finish(self):
        if self.finished:raise ValueError('live stream already finished')
        self.finished=True;m=self.metrics;m['memory_transactions']=self.count;m['transactions']=self.count+1
        m['dependency_edges']=self.count+(self.count+1 if self.phase else 0);m['census']['BARRIER']['transactions']=1
        sha=self.digest.hexdigest();p=self.session._process.stdin;p.write(f'END_LIVE {self.count} {sha}\n');p.flush()
        final=f'replay/{self.phase}/complete'
        batch=SimpleNamespace(batch_id=self.phase,logical_trace_sha256=self.logical,transaction_trace_sha256=sha,metrics=m,frontier_transactions=1,retain=(final,),frontier=(final,),transactions=(),completions=False)
        receipt=self.session._read_response(f'live batch {self.phase}')
        return self.session._validate_submission_receipt(batch,receipt)
