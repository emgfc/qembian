#!/usr/bin/env python3
"""Generate evidence-qualified EKA1 names; optionally apply to the open ROM.bin via Ghidra REST.
Does not recover SDK symbols. Reads the original ordinal map, never overwrites it.
"""
import argparse, hashlib, json, pathlib, re, struct, urllib.parse, urllib.request
ROOT = pathlib.Path(__file__).resolve().parents[1]

def build():
    original = json.loads((ROOT/'analysis/kernel-api-map.json').read_text())
    rom = (ROOT/'output/rom/ROM.bin').read_bytes()
    exports = {r['ordinal']: r for r in original['exports']}
    execs = {r['id']: r for r in original['exec']}
    def word(a): return struct.unpack_from('<I', rom, a-0x50000000)[0]
    for n,r in exports.items():
        assert word(0x50342068+4*(n-1)) == int(r['address'],16)
    for n,r in execs.items():
        assert word(0x503210dc+4*n) == (int(r['address'],16) if r['address'] else 0)
    # Verify the dispatcher instructions whose flag stripping justifies normalizing v6 IDs.
    assert word(0x50321494) == 0xe31c0502
    assert word(0x50321498) == 0xe3ccc5ff
    assert word(0x5032149c) == 0xe08fc10c
    refs = (ROOT/'references/eka2l1_svc.cpp').read_text().split('svc_register_funcs_v6 = {',1)[1].split('\n    };',1)[0]
    rows = []
    def add(addr,name,confidence,evidence,kind,number=None,prototype=None,**extra):
        row=dict(address=addr,name=name,confidence=confidence,evidence=evidence,kind=kind,number=number,**extra)
        if prototype: row['prototype']=prototype.format(name=name)
        rows.append(row)
    verified = {
        1:'Resolves chunk through export 50; returns +0x28 (base).',
        2:'Resolves chunk through export 50; returns +0x18 (size).',
        3:'Resolves chunk through export 50; returns +0x1c (maximum size).',
        0x18:'Mutex lookup export 245; returns +0x14 count.',
        0x19:'Mutex lookup export 245; invokes vtable +0x20, paired with signal +0x24.',
        0x1a:'Mutex lookup export 245; invokes vtable +0x24, paired with wait +0x20.',
        0x1b:'Process lookup export 293; returns +0x78 ID.',
        0x29:'Semaphore lookup export 339; returns +0x14 count.',
        0x2a:'Semaphore lookup export 339; invokes vtable +0x20.',
        0x2b:'Semaphore lookup export 339; invokes vtable +0x24.',
        0x2e:'Server lookup export 342; stores receive status/buffer and dequeues message; signals owner request semaphore.',
        0x2f:'Server lookup export 342; clears pending status and completes -3 (cancel), signals owner.',
        0x31:'Session lookup export 343; copies function/arguments/status into message and enqueues to server.',
        0x32:'Thread lookup export 441; returns +0x98 ID.',
        0x34:'Thread lookup export 441; invokes vtable +0x28; paired with suspend +0x2c.',
        0x35:'Thread lookup export 441; invokes vtable +0x2c; paired with resume +0x28.',
        0x36:'Thread lookup export 441; returns +0x1c priority.',
        0x3c:'Thread lookup export 441; returns request semaphore (+0x5c) count (+0x14).',
        0x3d:'Thread lookup export 441; returns +0x24 exit type.',
        0x46:'Resolves target thread; clears caller status pointer, writes completion and increments/wakes request semaphore.',
        0x47:'Timer lookup export 448; cancels timer, clears status and completes -3.',
        0x48:'Timer lookup export 448; rejects negative interval, stores request status and queues relative timer.',
        0x4d:'Current thread via export 72; waits on thread request semaphore at +0x5c, vtable +0x20.',
        0x6c:'Loads current thread at RAM 0x80006740, returns heap field +0x34.',
        0x6d:'Current thread via export 72; swaps heap field +0x34 and returns old value.',
        0x72:'Links supplied trap frame through thread +0x38; saves previous frame at +0x40 and handler at +0x48.',
        0x73:'Pops thread +0x38 trap chain using frame +0x40.',
        0x74:'Current-thread getter at +0x58, paired with setter 0x75.',
        0x75:'Current-thread setter at +0x58, paired with getter 0x74.',
        0x80:'Searches current-thread DLL TLS map at +0x40; returns entry payload or zero.',
        0x81:'Current-thread getter at +0x3c, paired with swapping setter 0x82.',
        0x82:'Swaps current-thread +0x3c trap handler, returns previous value.',
        0x8d:'Loads *r0, increments memory, returns previous value; locking relies on executive entry context.',
        0x8e:'Loads *r0, decrements memory, returns previous value; locking relies on executive entry context.',
        0xb0:'Chunk lookup export 50; returns +0x34 bottom.',
        0xb1:'Chunk lookup export 50; returns bottom + size (+0x34 + +0x18).',
        0xbc:'Returns literal ROM base 0x50000000.',
        0xc0:'Process lookup export 293; masks command-line descriptor length to 28 bits.',
        0xcc:'Same entry as export 153; data-cache clean, instruction-cache invalidate, write-buffer drain.'
    }
    prototypes = {0x6c:'void * {name}(void)',0x6d:'void * {name}(void *heap)',
      0x74:'void * {name}(void)',0x75:'void {name}(void *scheduler)',
      0x81:'void * {name}(void)',0x82:'void * {name}(void *handler)',
      0x8d:'int {name}(int *value)',0x8e:'int {name}(int *value)',0xbc:'void * {name}(void)'}
    for raw,n in re.findall(r'BRIDGE_REGISTER\((0x[0-9a-fA-F]+),\s*(\w+)\)',refs):
        full=int(raw,16); idx=full & ~0xc00000
        if idx not in execs or not execs[idx]['address']:continue
        r=execs[idx]; suffix=''.join(s.capitalize() for s in n.split('_'))
        confidence='behavior-corroborated' if idx in verified else 'reference-candidate'
        name=f'Eka1Exec{idx:02X}_{suffix}'+('' if idx in verified else '_Candidate')
        if idx==0xcc:continue  # dual-role entry below
        add(r['address'],name,confidence,verified.get(idx,'v6 reference plus verified dispatcher index normalization; exact operation not independently proven.'),'exec',idx,prototypes.get(idx),reference_name=n,reference_svc=raw,old_name=r['name'])
    # Stub behavior is proven by the instructions, without assigning an invented API identity.
    for idx,r in execs.items():
        if not r['address'] or int(r['address'],16)==0:continue
        a=int(r['address'],16)
        if word(a)==0xe3e00004 and word(a+4)==0xe1a0f00e:
            add(r['address'],f'Eka1Exec{idx:02X}_NotSupported','instruction-verified','MOV r0,#-5; MOV pc,lr. Reserved/unsupported behavior; original API unknown.','exec',idx,old_name=r['name'])
    helpers={
      6:('AtomicAddReturnNew','Saves interrupt mask with export 83; adds r1 to *r0; restores mask with export 332; returns new sum.','int {name}(int *value, int delta)'),
      44:('LookupLogicalChannelOrPanic','Exec 0x0c/0x0e use this before request/control dispatch.',None),
      50:('LookupChunkOrPanic','Exec 1/2/3/B0/B1 use this before accessing chunk fields.',None),
      72:('CurrentThread','Loads pointer from RAM 0x80006740; shared by heap, TLS and request-semaphore paths.','void * {name}(void)'),
      83:('DisableIrqFiqSaveMask','Assembly MRS CPSR; returns old CPSR & 0xc0; sets IRQ/FIQ disable bits. Decompiler longlong result is wrong.','unsigned int {name}(void)'),
      153:('Eka1ExecCC_InstructionMemoryBarrier','CP15 cache operations: range if length <=0x8000, otherwise whole-cache; drains write buffer; returns zero.','int {name}(void *base, unsigned int length)'),
      189:('LookupLibraryOrPanic','Library lookup/entry/filename executive wrappers use this.',None),
      245:('LookupMutexOrPanic','Mutex count/wait/signal executive wrappers use this.',None),
      262:('FatalFault','Disables interrupts, records category/reason in crash area, invokes platform shutdown path and does not return.',None),
      293:('LookupProcessOrPanic','Process ID and command-line executive wrappers use this.',None),
      332:('RestoreIrqFiqMask','Assembly replaces CPSR bits 0xc0 with r0 & 0xc0. Decompiler empty body is wrong.','void {name}(unsigned int mask)'),
      339:('LookupSemaphoreOrPanic','Semaphore count/wait/signal wrappers use this.',None),
      342:('LookupServerOrPanic','Server receive/cancel executive wrappers use this.',None),
      343:('LookupSessionOrPanic','Session-send wrapper resolves this object then enqueues a message.',None),
      432:('DispatchCurrentThreadException','Gets current thread; normal thread uses vtable +0x20; special thread type enters fatal path. Exact original name/signature not recovered.',None),
      441:('LookupThreadOrPanic','Thread ID/priority/resume/suspend wrappers use this.',None),
      448:('LookupTimerOrPanic','Timer cancel/after wrappers use this.',None)}
    for ordinal,(suffix,evidence,proto) in helpers.items():
        r=exports[ordinal]
        if suffix.startswith('Lookup'):
            proto='void * {name}(unsigned int handle)'
            evidence += ' Loads type-specific container, sets context arg zero, branches to shared checked lookup at 0x5033e108; null leads to error helper 0x5033e124.'
        add(r['address'],f'EkernExport{ordinal:03d}_{suffix}','behavior-corroborated',evidence,'export',ordinal,proto,old_name=r['name'])
    for addr,name,evidence in [
      ('0x50324714','Eka1_LookupHandleTyped','Chooses thread/process handle table using bit 0x40000000; typed container ID; pseudo-handles include 0xffff8001=current thread and 0xffff0000=current process.'),
      ('0x5033e124','Eka1_RaiseExecutiveError','Constructs exception arguments then invokes export 432 with selector 0x2f. Exact panic category not proven.'),
      ('0x503234c8','Eka1_CleanDataCacheRange','Reads CP15 cache geometry and cleans data-cache lines by MVA.'),
      ('0x50323510','Eka1_InvalidateInstructionCacheRange','Reads CP15 cache geometry and invalidates instruction-cache lines by MVA.')]:
        add(addr,name,'instruction-verified',evidence,'internal')
    # Thunk aliases retain ordinal and unique address; no new independent identity is claimed.
    named={r['number']:r for r in rows if r['kind']=='export'}
    for r in original['thunks']:
        if r['export_ordinal'] in named:
            target=named[r['export_ordinal']]
            add(r['address'],'Thunk_'+target['name']+'_'+r['address'][2:],'alias',f"Alias of {target['address']}; identity inherits target, not independent evidence.",'thunk',r['export_ordinal'],old_name=r['name'])
    assert len({r['address'] for r in rows})==len(rows)
    return dict(rom_sha256=hashlib.sha256(rom).hexdigest(),program='ROM.bin',notes='Behavioral labels are analyst names, not recovered SDK symbols. Candidate suffix means reference-only identity. ARM SVC control flags are not independent table indices.',functions=rows)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--apply',action='store_true');ap.add_argument('--server',default='http://127.0.0.1:8089');a=ap.parse_args()
    result=build();out=ROOT/'analysis/kernel-identifications.json';out.write_text(json.dumps(result,indent=2)+'\n')
    rows=result['functions'];(ROOT/'analysis/kernel-identifications.tsv').write_text('kind\tid_or_ordinal\taddress\tname\tconfidence\tevidence\n'+''.join('\t'.join(str(r.get(k,'')) for k in ['kind','number','address','name','confidence','evidence'])+'\n' for r in rows))
    if a.apply:
        def request(path,params=None):
            data=json.dumps(params).encode() if params is not None else None
            req=urllib.request.Request(a.server+'/'+path,data=data,headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=40) as f:return f.read().decode()
        current=json.loads(request('get_current_address'))
        if current.get('program','').split('/')[-1]!='ROM.bin':raise SystemExit('Refusing: active program is not ROM.bin')
        for r in rows:
            f=json.loads(request('get_function_by_address?address='+r['address']))
            if int(f.get('entry_point',f.get('address','0')),16)!=int(r['address'],16):raise SystemExit('Missing exact entry '+r['address'])
        log=[]
        for r in rows:
            actions=[('rename_function_by_address',{'address':r['address'],'function_address':r['address'],'new_name':r['name']}),('set_comment',{'address':r['address'],'comment_type':'plate','comment':'EKA1 identification ['+r['confidence']+']: '+r['evidence']+' Analyst label, not recovered SDK symbol. See eka1/analysis/kernel-entry-guide.md.'})]
            if 'prototype' in r:actions.append(('set_function_prototype',{'address':r['address'],'function_address':r['address'],'prototype':r['prototype']}))
            for endpoint,params in actions:
                reply=request(endpoint,params);log.append(dict(address=r['address'],endpoint=endpoint,reply=reply))
                (ROOT/'analysis/kernel-identifications-apply.json').write_text(json.dumps(log,indent=2)+'\n')
                response=json.loads(reply)
                if 'error' in response or response.get('status') in ('error','failed'):raise SystemExit(reply)
        print('Applied',len(rows),'annotations')
    from collections import Counter
    print(dict(Counter(r['kind'] for r in rows)),dict(Counter(r['confidence'] for r in rows)))
if __name__=='__main__':main()
