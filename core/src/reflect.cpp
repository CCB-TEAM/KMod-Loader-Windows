#include "reflect.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "kmod_common.h"

namespace Rf
{
namespace
{
struct Key { const char* name; int* target; };

Key* Keys(size_t& n)
{
    static Key k[] = {
        {"UObject.Flags", &L.uobj_flags}, {"UObject.Index", &L.uobj_index}, {"UObject.Class", &L.uobj_class},
        {"UObject.Name", &L.uobj_name}, {"UObject.Outer", &L.uobj_outer},
        {"UField.Next", &L.ufield_next},
        {"UStruct.SuperStruct", &L.ustruct_super}, {"UStruct.MinAlignment", &L.ustruct_minalign}, {"UStruct.Children", &L.ustruct_children},
        {"UStruct.ChildProperties", &L.ustruct_props}, {"UStruct.Size", &L.ustruct_size},
        {"UFunction.FunctionFlags", &L.ufunc_flags}, {"UFunction.ExecFunction", &L.ufunc_exec}, {"UFunction.ParamSize", &L.ufunc_paramsize},
        {"UClass.ClassDefaultObject", &L.uclass_cdo},
        {"FField.Class", &L.ffield_class}, {"FField.Next", &L.ffield_next}, {"FField.Name", &L.ffield_name},
        {"FFieldClass.Name", &L.ffieldclass_name},
        {"Property.ArrayDim", &L.prop_arraydim}, {"Property.ElementSize", &L.prop_elemsize},
        {"Property.PropertyFlags", &L.prop_flags}, {"Property.Offset_Internal", &L.prop_offset},
        {"BoolProperty.Base", &L.boolprop_base}, {"ByteProperty.Enum", &L.byteprop_enum}, {"EnumProperty.Base", &L.enumprop_base},
        {"StructProperty.Struct", &L.structprop_struct}, {"ArrayProperty.Inner", &L.arrayprop_inner},
        {"ObjectProperty.PropertyClass", &L.objprop_class}, {"ClassProperty.MetaClass", &L.classprop_meta},
        {"MapProperty.Base", &L.mapprop_base}, {"SetProperty.ElementProp", &L.setprop_elem},
        {"DelegateProperty.SignatureFunction", &L.delegprop_sig}, {"FName.Number", &L.fname_number},
    };
    n = sizeof(k) / sizeof(k[0]);
    return k;
}

Layout g_defaults;                                  // what the SDK compiled into this core assumes
std::string g_diff;

long long JsonNum(const std::string& js, const char* key, long long def)
{
    size_t k = js.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return def;
    size_t c = js.find(':', k);
    return c == std::string::npos ? def : atoll(js.c_str() + c + 1);
}
}

int Load(const std::string& json)
{
    size_t n;
    Key* keys = Keys(n);
    g_defaults = Layout();
    int diffs = 0;
    g_diff.clear();
    for (size_t i = 0; i < n; i++)
    {
        long long v = JsonNum(json, keys[i].name, -9999);
        if (v == -9999 || v < 0 || v > 0x4000) continue;                   // not reported or not believable: keep the SDK value
        if (*keys[i].target != (int)v)
        {
            char b[96]; snprintf(b, sizeof(b), "%s 0x%x->0x%x ", keys[i].name, *keys[i].target, (int)v);
            g_diff += b;
            *keys[i].target = (int)v;
            diffs++;
        }
    }
    g_adapted = diffs > 0;
    return diffs;
}

std::string Describe() { return g_diff; }

std::string Str(const FName& n)
{
    std::string s = n.ToString();
    int num = At<int32_t>(&n, L.fname_number);
    if (num > 0) s += "_" + std::to_string(num - 1);
    return s;
}

std::string FullName(const UObject* o)
{
    if (!o || !ClassOf(o)) return "None";
    std::string temp;
    int guard = 0;
    for (UObject* out = OuterOf(o); out && guard < 64; out = OuterOf(out), guard++) temp = NameStr(out) + "." + temp;
    return NameStr(ClassOf(o)) + " " + temp + NameStr(o);
}

std::string PathName(const UObject* o)
{
    std::vector<const UObject*> chain;
    for (const UObject* c = o; c && chain.size() < 32; c = OuterOf(c)) chain.push_back(c);
    std::string path;
    for (size_t i = chain.size(); i-- > 0;)
    {
        std::string n = InSDKUtils::GetNameByIndex(At<FName>(chain[i], L.uobj_name).GetDisplayIndex());
        if (i + 1 == chain.size()) path = n;
        else path += (i + 2 == chain.size() ? "." : ":") + n;
    }
    return path;
}

bool IsA(const UObject* o, const UClass* cls)
{
    if (!o || !cls) return false;
    int guard = 0;
    for (const UStruct* c = ClassOf(o); c && guard < 128; c = SuperOf(c), guard++)
        if (c == reinterpret_cast<const UStruct*>(cls)) return true;
    return false;
}

bool IsCdo(const UObject* o) { return (FlagsOf(o) & 0x10) != 0; }

namespace
{
template <class F> void ForEach(F&& f)
{
    TUObjectArray* a = InSDKUtils::GetGObjects();
    if (!a) return;
    for (int32_t i = 0; i < a->Num(); i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || !ClassOf(o)) continue;
        if (f(o)) break;
    }
}
}

UClass* ClassClass()
{
    static UClass* c = nullptr;
    if (c) return c;
    ForEach([&](UObject* o) { if (ClassOf(o) == reinterpret_cast<UClass*>(o) && NameStr(o) == "Class") { c = reinterpret_cast<UClass*>(o); return true; } return false; });
    return c;
}

UClass* FunctionClass()
{
    static UClass* c = nullptr;
    if (c) return c;
    UClass* cc = ClassClass();
    ForEach([&](UObject* o) { if (ClassOf(o) == cc && NameStr(o) == "Function") { c = reinterpret_cast<UClass*>(o); return true; } return false; });
    return c;
}

UClass* FindClass(const std::string& name)
{
    static std::unordered_map<std::string, UClass*> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    UClass* cc = ClassClass();
    UClass* found = nullptr;
    if (cc)
        ForEach([&](UObject* o)
        {
            if (!IsA(o, cc)) return false;                                  // a UClass (or a Blueprint class)
            if (NameStr(o) == name) { found = reinterpret_cast<UClass*>(o); return true; }
            return false;
        });
    if (found) cache[name] = found;
    return found;
}

FProperty* FindProp(const UStruct* s, const std::string& name)
{
    int guard = 0;
    for (const UStruct* c = s; c && guard < 128; c = SuperOf(c), guard++)
        for (FField* f = PropsOf(c); f; f = NextOf(f))
            if (NameStr(f) == name) return reinterpret_cast<FProperty*>(f);
    return nullptr;
}

UFunction* FindFunc(const UStruct* s, const std::string& name)
{
    UClass* fcls = FunctionClass();
    int guard = 0;
    for (const UStruct* c = s; c && guard < 128; c = SuperOf(c), guard++)
        for (UField* f = ChildrenOf(c); f; f = NextOf(f))
            if (ClassOf(f) == fcls && NameStr(f) == name) return reinterpret_cast<UFunction*>(f);
    return nullptr;
}

UObject* CdoFor(const char* cls)
{
    UClass* c = FindClass(cls);
    return c ? CdoOf(c) : nullptr;
}

UObject* FindFirst(const char* cls)
{
    UClass* c = FindClass(cls);
    UObject* found = nullptr;
    if (c) ForEach([&](UObject* o) { if (!IsCdo(o) && IsA(o, c)) { found = o; return true; } return false; });
    return found;
}

UObject* ObjProp(const UObject* o, const char* prop)
{
    if (!o) return nullptr;
    FProperty* p = FindProp(reinterpret_cast<UStruct*>(ClassOf(o)), prop);
    return p ? At<UObject*>(o, OffsetOf(p)) : nullptr;
}

Args::Args(UFunction* fn) : fn_(fn)
{
    if (!fn) return;
    size_t sz = ParamSizeOf(fn);
    for (FField* f = PropsOf(reinterpret_cast<UStruct*>(fn)); f; f = NextOf(f))
    {
        FProperty* p = reinterpret_cast<FProperty*>(f);
        if (PropFlagsOf(p) & 0x80) { int64_t end = (int64_t)OffsetOf(p) + (int64_t)ElemSizeOf(p) * std::max(1, ArrayDimOf(p)); if (end >= 0 && end <= 0x10000) sz = std::max<size_t>(sz, (size_t)end); }
    }
    buf_.assign(sz + 64, 0);
}

int Args::Find(const char* name, size_t size)
{
    if (!fn_) return -1;
    for (FField* f = PropsOf(reinterpret_cast<UStruct*>(fn_)); f; f = NextOf(f))
    {
        FProperty* p = reinterpret_cast<FProperty*>(f);
        if ((PropFlagsOf(p) & 0x80) && NameStr(f) == name)
        {
            int off = OffsetOf(p);
            return (size_t)off + size <= buf_.size() ? off : -1;
        }
    }
    return -1;
}

bool Args::Call(UObject* context)
{
    if (!fn_ || !context) return false;
    context->ProcessEvent(fn_, buf_.data());
    return true;
}

Args Prepare(const char* cls, const char* func)
{
    UClass* c = FindClass(cls);
    return Args(c ? FindFunc(reinterpret_cast<UStruct*>(c), func) : nullptr);
}
}
