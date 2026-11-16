#include "proven_write_runtime.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
namespace P=Vulkan::ProvenWrites;
namespace R=Vulkan::ProvenWritesRuntime;
unsigned checks{};
void Check(bool good,const char* what){++checks;if(!good)throw std::runtime_error(what);}
struct Builder {
    using U=P::U32;
    std::vector<U> decl,body,annotations;
    bool int64{};
    U next=1;
    U void_t=Id(),u32_t=Id(),v3_t=Id(),arr_t=Id(),struct_t=Id(),ptr_struct_t=Id(),ptr_u32_t=Id(),ptr_input_t=Id(),
      out=Id(),input=Id(),gid=Id(),local=Id(),group=Id(),function_t=Id(),main=Id(),label=Id();
    U zero,one;
    U Id(){return next++;}
    static void Op(std::vector<U>& dest,spv::Op op,std::initializer_list<U> a){dest.push_back((U(a.size()+1)<<16)|op);dest.insert(dest.end(),a);}
    U Constant(U n){const U id=Id();Op(decl,spv::OpConstant,{u32_t,id,n});return id;}
    U Binary(spv::Op op,U a,U b){U id=Id();Op(body,op,{u32_t,id,a,b});return id;}
    U Global(U axis=0){U v=Id(),id=Id();Op(body,spv::OpLoad,{v3_t,v,gid});Op(body,spv::OpCompositeExtract,{u32_t,id,v,axis});return id;}
    U Load(){U ptr=Id(),id=Id();Op(body,spv::OpAccessChain,{ptr_u32_t,ptr,input,zero,zero});Op(body,spv::OpLoad,{u32_t,id,ptr});return id;}
    void Store(U index,bool atomic=false){U ptr=Id();Op(body,spv::OpAccessChain,{ptr_u32_t,ptr,out,zero,index});if(atomic){U id=Id();Op(body,spv::OpAtomicIAdd,{u32_t,id,ptr,one,zero,one});}else Op(body,spv::OpStore,{ptr,one});}
    Builder(U binding=0) {
        Op(decl,spv::OpTypeVoid,{void_t});Op(decl,spv::OpTypeInt,{u32_t,32,0});Op(decl,spv::OpTypeVector,{v3_t,u32_t,3});
        Op(decl,spv::OpTypeRuntimeArray,{arr_t,u32_t});Op(decl,spv::OpTypeStruct,{struct_t,arr_t});
        Op(decl,spv::OpTypePointer,{ptr_struct_t,spv::StorageClassStorageBuffer,struct_t});
        Op(decl,spv::OpTypePointer,{ptr_u32_t,spv::StorageClassStorageBuffer,u32_t});
        Op(decl,spv::OpTypePointer,{ptr_input_t,spv::StorageClassInput,v3_t});Op(decl,spv::OpTypeFunction,{function_t,void_t});
        Op(decl,spv::OpVariable,{ptr_struct_t,out,spv::StorageClassStorageBuffer});Op(decl,spv::OpVariable,{ptr_struct_t,input,spv::StorageClassStorageBuffer});
        for(U v:{gid,local,group})Op(decl,spv::OpVariable,{ptr_input_t,v,spv::StorageClassInput});
        zero=Constant(0);one=Constant(1);
        Op(body,spv::OpFunction,{void_t,main,spv::FunctionControlMaskNone,function_t});Op(body,spv::OpLabel,{label});
    }
    std::vector<U> Finish(U binding=0){
        std::vector<U> code{spv::MagicNumber,0x00010300,0,next,0};
        Op(code,spv::OpCapability,{spv::CapabilityShader});Op(code,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
        if(int64)code.insert(code.begin()+7,{(2u<<16)|spv::OpCapability,spv::CapabilityInt64});
        Op(code,spv::OpEntryPoint,{spv::ExecutionModelGLCompute,main,0x6e69616d,0,gid,local,group});
        Op(code,spv::OpExecutionMode,{main,spv::ExecutionModeLocalSize,64,1,1});
        Op(code,spv::OpDecorate,{arr_t,spv::DecorationArrayStride,4});Op(code,spv::OpDecorate,{struct_t,spv::DecorationBlock});
        Op(code,spv::OpMemberDecorate,{struct_t,0,spv::DecorationOffset,0});
        for(U v:{out,input})Op(code,spv::OpDecorate,{v,spv::DecorationDescriptorSet,0});
        Op(code,spv::OpDecorate,{out,spv::DecorationBinding,binding});Op(code,spv::OpDecorate,{input,spv::DecorationBinding,binding+1});
        Op(code,spv::OpDecorate,{gid,spv::DecorationBuiltIn,spv::BuiltInGlobalInvocationId});
        Op(code,spv::OpDecorate,{local,spv::DecorationBuiltIn,spv::BuiltInLocalInvocationId});
        Op(code,spv::OpDecorate,{group,spv::DecorationBuiltIn,spv::BuiltInWorkgroupId});
        code.insert(code.end(),annotations.begin(),annotations.end());code.insert(code.end(),decl.begin(),decl.end());code.insert(code.end(),body.begin(),body.end());
        Op(code,spv::OpReturn,{});Op(code,spv::OpFunctionEnd,{});code[3]=next;return code;
    }
};
int main(){try{
    const P::Launch good{true,{7,1,1},{64,1,1},{}};
    using U=P::U32;
    std::vector<U> samples{0,1,2,31,63,64,65,255,256,1023,65535,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
    std::mt19937 rng(0x5148);for(unsigned n=0;n<256;++n)samples.push_back(rng());
    // Independent concrete unsigned C++ arithmetic is checked against the abstract footprint.
    // These are deliberately synthetic address-expression tests, separate from real-module GPU tests.
    for(unsigned kind=0;kind<15;++kind){
        Builder b;auto x=b.Global(),v=b.Load(),index=x;
        auto k=b.Constant(31),large=b.Constant(0xfffffff0),shift=b.Constant(3);
        switch(kind){
        case 0:index=b.Binary(spv::OpIMul,x,b.Constant(3));break;
        case 1:index=b.Binary(spv::OpBitwiseAnd,v,k);break;
        case 2:index=b.Binary(spv::OpUMod,v,b.Constant(97));break;
        case 3:index=b.Binary(spv::OpBitwiseAnd,b.Binary(spv::OpIAdd,v,large),k);break;
        case 4:index=b.Binary(spv::OpBitwiseAnd,b.Binary(spv::OpISub,v,x),k);break;
        case 5:index=b.Binary(spv::OpBitwiseAnd,b.Binary(spv::OpIMul,v,x),k);break;
        case 6:index=b.Binary(spv::OpShiftRightLogical,b.Binary(spv::OpBitwiseAnd,v,b.Constant(255)),shift);break;
        case 7:index=b.Binary(spv::OpShiftLeftLogical,x,shift);break;
        case 8:index=b.Binary(spv::OpBitwiseOr,b.Binary(spv::OpBitwiseAnd,v,k),x);break;
        case 9:index=b.Binary(spv::OpBitwiseXor,b.Binary(spv::OpBitwiseAnd,v,k),x);break;
        case 10:index=b.Binary(spv::OpUDiv,b.Binary(spv::OpBitwiseAnd,v,b.Constant(1023)),b.Constant(7));break;
        case 11:index=b.Binary(spv::OpIAdd,x,b.Constant(17));break;
        case 12:index=b.Binary(spv::OpIAdd,v,x);break; // Wrapping/data-dependent: must retain full range.
        case 13:index=v;break;
        case 14:index=b.Binary(spv::OpIAdd,x,b.Global(1));break;
        }
        b.Store(index,kind==1);const auto code=b.Finish();P::Module m(code);const auto r=m.Evaluate(good);
        Check(r.valid,"synthetic module rejected");
        if(kind==12||kind==13)Check(r.end[0]==P::Unknown,"unbounded/overflowing address accepted");
        else Check(r.end[0]!=P::Unknown,"known expression not bounded");
        for(U g:{0u,1u,63u,64u,127u,447u})for(U v:samples){
            U at=g;
            switch(kind){
            case 0:at=g*3;break;case 1:at=v&31;break;case 2:at=v%97;break;
            case 3:at=(v+0xfffffff0u)&31;break;case 4:at=(v-g)&31;break;case 5:at=(v*g)&31;break;
            case 6:at=(v&255)>>3;break;case 7:at=g<<3;break;case 8:at=(v&31)|g;break;
            case 9:at=(v&31)^g;break;case 10:at=(v&1023)/7;break;case 11:at=g+17;break;
            case 12:at=v+g;break;case 13:at=v;break;case 14:at=g;break;
            }
            Check(r.end[0]==P::Unknown||P::U64(at)*4+4<=r.end[0],"concrete write escaped abstract footprint");
        }
        const auto indirect=m.Evaluate({});
        if(kind==1||kind==2||kind==3||kind==6||kind==10)Check(indirect.valid&&indirect.end[0]!=P::Unknown,"launch-independent bound lost");
        auto bad=good;bad.start[1]=1;Check(!m.Evaluate(bad).valid,"nonzero start accepted");
        bad=good;bad.local[0]=32;Check(!m.Evaluate(bad).valid,"local mismatch accepted");
        bad=good;bad.groups[0]=0;Check(!m.Evaluate(bad).valid,"zero-group launch accepted");
        auto changed=code;changed.back()^=1;Check(!m.Matches(changed),"module mismatch accepted");
        if(kind==0){std::ofstream f("synthetic-address.spv",std::ios::binary);f.write((char*)code.data(),code.size()*4);}
    }
    // An unhandled memory-copy operation invalidates the proof, even alongside a bounded store.
    {Builder b;auto x=b.Global();b.Store(x);U p=b.Id(),q=b.Id();Builder::Op(b.body,spv::OpAccessChain,{b.ptr_u32_t,p,b.out,b.zero,b.zero});Builder::Op(b.body,spv::OpAccessChain,{b.ptr_u32_t,q,b.input,b.zero,b.zero});Builder::Op(b.body,spv::OpCopyMemory,{p,q});auto c=b.Finish();P::Module m(c);Check(!m.Evaluate(good).valid,"copy-memory silently ignored");}
    // A second store at a distant address must be included regardless of first-store order.
    {Builder b;b.Store(b.Global());b.Store(b.Constant(8191));auto c=b.Finish();P::Module m(c);Check(m.Evaluate(good).end[0]==32768,"second store omitted");}
    // Exhaust the entire two-input byte domain using independent concrete arithmetic.
    for(unsigned op=0;op<6;++op){
        Builder b;auto mask=b.Constant(255),a=b.Binary(spv::OpBitwiseAnd,b.Load(),mask);
        auto d=b.Binary(spv::OpBitwiseAnd,b.Global(),mask);
        const auto opcode=std::array{spv::OpIAdd,spv::OpIMul,spv::OpBitwiseAnd,spv::OpBitwiseOr,spv::OpBitwiseXor,spv::OpISub}[op];
        auto index=b.Binary(opcode,a,d);b.Store(index);auto c=b.Finish();P::Module m(c);
        const auto r=m.Evaluate(good);Check(r.valid,"byte-domain fixture");
        for(U a=0;a<256;++a)for(U d=0;d<256;++d){
            U at=0;switch(op){case 0:at=a+d;break;case 1:at=a*d;break;case 2:at=a&d;break;case 3:at=a|d;break;case 4:at=a^d;break;case 5:at=a-d;break;}
            Check(r.end[0]==P::Unknown||P::U64(at)*4+4<=r.end[0],"byte-domain counterexample");
        }
    }
    // Cyclic loop values retain full uncertainty; masking can still prove a finite address.
    for(bool masked:{false,true}){
        Builder b;U bool_t=b.Id();Builder::Op(b.decl,spv::OpTypeBool,{bool_t});
        U head=b.Id(),work=b.Id(),cont=b.Id(),merge=b.Id(),counter=b.Id(),next=b.Id(),cond=b.Id();
        Builder::Op(b.body,spv::OpBranch,{head});Builder::Op(b.body,spv::OpLabel,{head});
        Builder::Op(b.body,spv::OpPhi,{b.u32_t,counter,b.zero,b.label,next,cont});
        Builder::Op(b.body,spv::OpLoopMerge,{merge,cont,spv::LoopControlMaskNone});Builder::Op(b.body,spv::OpBranch,{work});
        Builder::Op(b.body,spv::OpLabel,{work});b.Store(masked?b.Binary(spv::OpBitwiseAnd,counter,b.Constant(31)):counter);
        Builder::Op(b.body,spv::OpBranch,{cont});Builder::Op(b.body,spv::OpLabel,{cont});
        Builder::Op(b.body,spv::OpIAdd,{b.u32_t,next,counter,b.one});
        Builder::Op(b.body,spv::OpULessThan,{bool_t,cond,next,b.Constant(128)});
        Builder::Op(b.body,spv::OpBranchConditional,{cond,head,merge});Builder::Op(b.body,spv::OpLabel,{merge});
        auto c=b.Finish();P::Module m(c);auto r=m.Evaluate(good);Check(r.valid,"cyclic module fixture");
        Check(r.end[0]==(masked?128:P::Unknown),"loop counter was guessed");
        if(masked){std::ofstream f("synthetic-loop.spv",std::ios::binary);f.write((char*)c.data(),c.size()*4);}
    }
    // Real structure offsets and vector-store widths belong in the byte footprint.
    {Builder b;b.Store(b.Global());auto c=b.Finish();for(size_t p=5;p<c.size();p+=c[p]>>16)if((c[p]&65535)==spv::OpMemberDecorate&&c[p+1]==b.struct_t)c[p+4]=64;
     P::Module m(c);Check(m.Evaluate(good).end[0]==1856,"struct offset lost");}
    {Builder b;U vec=b.Id(),array=b.Id(),structure=b.Id(),ptr_struct=b.Id(),ptr_vec=b.Id(),variable=b.Id(),value=b.Id(),pointer=b.Id();
     Builder::Op(b.decl,spv::OpTypeVector,{vec,b.u32_t,4});Builder::Op(b.decl,spv::OpTypeRuntimeArray,{array,vec});Builder::Op(b.decl,spv::OpTypeStruct,{structure,array});
     Builder::Op(b.decl,spv::OpTypePointer,{ptr_struct,spv::StorageClassStorageBuffer,structure});Builder::Op(b.decl,spv::OpTypePointer,{ptr_vec,spv::StorageClassStorageBuffer,vec});
     Builder::Op(b.decl,spv::OpVariable,{ptr_struct,variable,spv::StorageClassStorageBuffer});
     Builder::Op(b.annotations,spv::OpDecorate,{array,spv::DecorationArrayStride,16});Builder::Op(b.annotations,spv::OpDecorate,{structure,spv::DecorationBlock});
     Builder::Op(b.annotations,spv::OpMemberDecorate,{structure,0,spv::DecorationOffset,0});Builder::Op(b.annotations,spv::OpDecorate,{variable,spv::DecorationBinding,2});Builder::Op(b.annotations,spv::OpDecorate,{variable,spv::DecorationDescriptorSet,0});
     const U index=b.Global();Builder::Op(b.body,spv::OpCompositeConstruct,{vec,value,b.one,b.one,b.one,b.one});
     Builder::Op(b.body,spv::OpAccessChain,{ptr_vec,pointer,variable,b.zero,index});Builder::Op(b.body,spv::OpStore,{pointer,value});
     auto c=b.Finish();P::Module m(c);auto r=m.Evaluate(good);Check(r.valid&&r.end[2]==7168,"vector width/stride lost");Check(r.end[0]==0,"no-write binding changed");
     std::ofstream f("synthetic-vector.spv",std::ios::binary);f.write((char*)c.data(),c.size()*4);}
    // 64-bit exact overflow and an unbounded large index must not become small prefixes.
    for(bool wrapped:{false,true}){Builder b;b.int64=true;U type=b.Id(),high=b.Id(),one=b.Id(),sum=b.Id();
     Builder::Op(b.decl,spv::OpTypeInt,{type,64,0});Builder::Op(b.decl,spv::OpConstant,{type,high,0xffffffff,0xffffffff});Builder::Op(b.decl,spv::OpConstant,{type,one,1,0});
     Builder::Op(b.body,spv::OpIAdd,{type,sum,high,one});b.Store(wrapped?sum:high);auto c=b.Finish();P::Module m(c);auto r=m.Evaluate(good);
     Check(r.valid&&r.end[0]==(wrapped?4:P::Unknown),"64-bit wrapping/range proof");
     if(wrapped){std::ofstream f("synthetic-u64.spv",std::ios::binary);f.write((char*)c.data(),c.size()*4);}}
    // Workgroup/private stores do not mark guest memory, while regular stores still do.
    {Builder b;U pointer=b.Id(),var=b.Id();Builder::Op(b.decl,spv::OpTypePointer,{pointer,spv::StorageClassWorkgroup,b.u32_t});Builder::Op(b.decl,spv::OpVariable,{pointer,var,spv::StorageClassWorkgroup});
     Builder::Op(b.body,spv::OpStore,{var,b.one});b.Store(b.Global());auto c=b.Finish();P::Module m(c);Check(m.Evaluate(good).end[0]==1792,"local store tainted guest footprint");}
    // All helper functions are scanned. Scalars remain unknown; pointer arguments do not
    // become trusted aliases merely because the one test call passes a known descriptor.
    for(bool pointer_argument:{false,true}){
        Builder b;U helper_type=b.Id(),helper=b.Id(),parameter=b.Id(),block=b.Id(),call=b.Id();
        Builder::Op(b.decl,spv::OpTypeFunction,{helper_type,b.void_t,pointer_argument?b.ptr_u32_t:b.u32_t});
        U arg=b.Load();
        if(pointer_argument){arg=b.Id();Builder::Op(b.body,spv::OpAccessChain,{b.ptr_u32_t,arg,b.out,b.zero,b.zero});}
        Builder::Op(b.body,spv::OpFunctionCall,{b.void_t,call,helper,arg});
        auto code=b.Finish();std::vector<U> h;
        Builder::Op(h,spv::OpFunction,{b.void_t,helper,spv::FunctionControlMaskNone,helper_type});
        Builder::Op(h,spv::OpFunctionParameter,{pointer_argument?b.ptr_u32_t:b.u32_t,parameter});Builder::Op(h,spv::OpLabel,{block});
        if(pointer_argument)Builder::Op(h,spv::OpStore,{parameter,b.one});
        else {U mask=b.Id(),index=b.Id(),ptr=b.Id();
            // Constants must precede all functions; reuse an existing all-ones mask (1)
            // so the helper's scalar parameter is provably restricted to two words.
            Builder::Op(h,spv::OpBitwiseAnd,{b.u32_t,index,parameter,b.one});
            Builder::Op(h,spv::OpAccessChain,{b.ptr_u32_t,ptr,b.out,b.zero,index});Builder::Op(h,spv::OpStore,{ptr,b.one});}
        Builder::Op(h,spv::OpReturn,{});Builder::Op(h,spv::OpFunctionEnd,{});
        code.insert(code.end(),h.begin(),h.end());code[3]=b.next;
        P::Module module(code);auto r=module.Evaluate(good);
        if(pointer_argument)Check(!r.valid,"pointer parameter incorrectly trusted");
        else {Check(r.valid&&r.end[0]==8,"helper-function writes omitted");std::ofstream f("synthetic-helper.spv",std::ios::binary);f.write((char*)code.data(),code.size()*4);}
    }
    // Cached modules are compared byte-for-byte, including when the pointer/size stay the same.
    {Builder b;U at=b.Constant(7);b.Store(at);auto c=b.Finish();R::Cache cache;Check(cache.Get(c,good).end[0]==32,"cache initial bound");
     for(size_t p=5;p<c.size();p+=c[p]>>16)if((c[p]&65535)==spv::OpConstant&&c[p+2]==at)c[p+3]=1023;
     Check(cache.Get(c,good).end[0]==4096,"same-pointer stale cache proof");}
    {Builder b;b.Store(b.Global());auto c=b.Finish();R::Cache cache;
     for(unsigned round=0;round<3;++round)for(U groups=1;groups<=17;++groups){auto dims=good;dims.groups[0]=groups;Check(cache.Get(c,dims).end[0]==P::U64(groups)*256,"launch-shape cache stale/eviction");}}
    // Context and descriptor guards, including aligned allocation-page invariants and nested scopes.
    {Builder b; b.Store(b.Global());auto c=b.Finish(12);int identity{},other{};R::launch=good;
     R::BeginBindings();R::Prepare(&identity,0x5ab84461,5,true,false,12,256,16384,c);
     auto query=[&](P::U64 address=0x10000,P::U64 size=65536,bool odd=false,bool format=false){return R::Bound(&identity,0x5ab84461,5,0,address,size,odd,format);};
     Check(query()==1792,"stage binding-base bound");Check(!query(0x10004),"unaligned address accepted");
     Check(!query(0x10000,1024),"descriptor underrun accepted");Check(!query(0x10000,65535),"partial descriptor accepted");
     Check(!query(0x10000,65536,true),"swizzle accepted");Check(!query(0x10000,65536,false,true),"formatted accepted");
     Check(!R::Bound(&other,0x5ab84461,5,0,0x10000,65536,false,false),"wrong stage identity");
     {R::LaunchScope scope({});Check(!query(),"nested launch stale proof");}Check(query().has_value(),"launch restore");
     for(auto hash:{0xc4947c72ull,0x252ccde6ull,0xdeadbeefull}){R::Prepare(&identity,hash,5,true,false,12,256,16384,c);Check(!R::Bound(&identity,hash,5,0,0x10000,65536,false,false),"old/special/unapproved route accepted");}
     R::Prepare(&identity,0x5ab84461,5,true,true,12,256,16384,c);Check(!query(),"ordered/DMA exclusion ignored");
     R::Prepare(&identity,0x5ab84461,5,true,false,12,32768,16384,c);Check(!query(),"page-alignment proof missing");
    }
    // Reentrant binding of the same program/launch with a different module must not
    // leave its narrower proof installed for the suspended outer dispatch.
    {Builder outer;outer.Store(outer.Global());auto wide=outer.Finish();Builder inner;inner.Store(inner.Constant(7));auto narrow=inner.Finish();int identity{};
     R::LaunchScope outer_scope(good);R::Prepare(&identity,0x5ab84461,5,true,false,0,256,16384,wide);
     Check(R::Bound(&identity,0x5ab84461,5,0,0x10000,65536,false,false)==1792,"outer dispatch proof");
     {R::LaunchScope inner_scope(good);R::BeginBindings();R::Prepare(&identity,0x5ab84461,5,true,false,0,256,16384,narrow);
      Check(R::Bound(&identity,0x5ab84461,5,0,0x10000,65536,false,false)==32,"inner dispatch proof");}
     Check(R::Bound(&identity,0x5ab84461,5,0,0x10000,65536,false,false)==1792,"reentrant proof context not restored");}
    // Multidimensional launches and multi-wave local sizes use actual IDs, not wave order.
    {Builder b;auto x=b.Global(0),y=b.Global(1),z=b.Global(2);
     b.Store(b.Binary(spv::OpIAdd,b.Binary(spv::OpIAdd,x,b.Binary(spv::OpIMul,y,b.Constant(512))),b.Binary(spv::OpIMul,z,b.Constant(2048))));
     auto c=b.Finish();P::Module m(c);auto dims=good;dims.groups={7,3,2};const auto r=m.Evaluate(dims);Check(r.valid&&r.end[0]==14080,"3D launch bound");
     for(unsigned z=0;z<2;++z)for(unsigned y=0;y<3;++y)for(unsigned x=0;x<448;++x)Check(P::U64(x+y*512+z*2048)*4+4<=r.end[0],"3D address escaped");
     for(size_t p=5;p<c.size();p+=c[p]>>16)if((c[p]&65535)==spv::OpExecutionMode){c[p+3]=16;c[p+4]=4;c[p+5]=2;}
     P::Module multi(c);dims.local={16,4,2};Check(multi.Evaluate(dims).valid,"128-lane local shape rejected");}
    std::cout<<"PASS "<<checks<<" arithmetic/address/cache/context checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
