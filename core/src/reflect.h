// Runtime-layout reflection. The structure layouts of the engine's reflection data (UObject, UStruct, UFunction, FField,
// FProperty and its subclasses) are NOT taken from the SDK that was compiled into the core: they live in `Rf::L`, filled
// with the SDK's values by default and overridden by the layout that the detector (libkmodmd7.so, see md7/) found in the
// running game. Everything that walks reflection data goes through the accessors below, so a game build whose layout moved
// still works without rebuilding the core.
//
// The SDK classes (UObject, UStruct, FProperty ...) are used here only as opaque pointer types.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"

namespace Rf
{
using namespace SDK;

struct Layout
{
    // Plain numbers (UE 5.6 on x64; identical to the values the detector reported for the Android build). They are NOT taken from the
    // SDK's C++ classes: MSVC lays out derived classes differently from the clang layout the SDK was generated for.
    // UObject
    int uobj_flags = 0x08, uobj_index = 0x0C, uobj_class = 0x10, uobj_name = 0x18, uobj_outer = 0x20;
    // UField / UStruct / UFunction / UClass
    int ufield_next = 0x28;
    int ustruct_super = 0x40, ustruct_children = 0x48, ustruct_props = 0x50, ustruct_size = 0x58;
    int ustruct_minalign = 0x5C;                                      // reported by the detector ("UStruct.MinAlignment")
    int ustruct_script = 0x60;                                        // UStruct::Script (TArray<uint8>): follows MinAlignment; verified when frames are learned
    int ufunc_flags = 0xB0, ufunc_exec = 0xD8, ufunc_paramsize = 0xB6;
    int uclass_cdo = 0x110;
    // FField / FFieldClass
    int ffield_class = 0x08, ffield_next = 0x18, ffield_name = 0x20;
    int ffieldclass_name = 0x00;
    // FProperty and subclasses
    int prop_arraydim = 0x30, prop_elemsize = 0x34, prop_flags = 0x38, prop_offset = 0x44;
    int boolprop_base = 0x70;                                         // FieldSize, ByteOffset, ByteMask, FieldMask
    int byteprop_enum = 0x70;
    int enumprop_base = 0x70;                                         // underlying property, then the enum
    int structprop_struct = 0x70;
    int arrayprop_inner = 0x78;
    int objprop_class = 0x70;
    int classprop_meta = 0x78;
    int mapprop_base = 0x70;                                          // key property, then the value property
    int setprop_elem = 0x70;
    int delegprop_sig = 0x70;
    // FName
    int fname_number = 4;
};
inline Layout L;
inline bool g_adapted = false;                      // true when at least one value came from the detector and differs from the SDK

// Applies a layout.json written by the detector (flat {"Key": number}). Returns how many values differ from the SDK defaults.
int Load(const std::string& json);
std::string Describe();                             // one line per value that differs (for the log)

template <class T> inline T& At(const void* p, int off) { return *reinterpret_cast<T*>(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(p)) + off); }

// ---- UObject ---------------------------------------------------------------------------------------
inline UClass* ClassOf(const UObject* o) { return At<UClass*>(o, L.uobj_class); }
inline UObject* OuterOf(const UObject* o) { return At<UObject*>(o, L.uobj_outer); }
inline int32_t IndexOf(const UObject* o) { return At<int32_t>(o, L.uobj_index); }
inline uint32_t FlagsOf(const UObject* o) { return At<uint32_t>(o, L.uobj_flags); }
inline FName NameOf(const UObject* o) { return At<FName>(o, L.uobj_name); }

// ---- UStruct / UField / UFunction / UClass ---------------------------------------------------------
inline UStruct* SuperOf(const UStruct* s) { return At<UStruct*>(s, L.ustruct_super); }
inline UField* ChildrenOf(const UStruct* s) { return At<UField*>(s, L.ustruct_children); }
inline FField* PropsOf(const UStruct* s) { return At<FField*>(s, L.ustruct_props); }
inline UField* NextOf(const UField* f) { return At<UField*>(f, L.ufield_next); }
inline uint32_t FuncFlagsOf(const UFunction* f) { return At<uint32_t>(f, L.ufunc_flags); }
inline uint16_t ParamSizeOf(const UFunction* f) { return At<uint16_t>(f, L.ufunc_paramsize); }
inline UObject* CdoOf(const UClass* c) { return At<UObject*>(c, L.uclass_cdo); }

// ---- FField / FProperty ----------------------------------------------------------------------------
inline FField* NextOf(const FField* f) { return At<FField*>(f, L.ffield_next); }
inline FName NameOf(const FField* f) { return At<FName>(f, L.ffield_name); }
inline FFieldClass* ClassOf(const FField* f) { return At<FFieldClass*>(f, L.ffield_class); }
inline FName NameOf(const FFieldClass* c) { return At<FName>(c, L.ffieldclass_name); }
inline int32_t ArrayDimOf(const FProperty* p) { return At<int32_t>(p, L.prop_arraydim); }
inline int32_t ElemSizeOf(const FProperty* p) { return At<int32_t>(p, L.prop_elemsize); }
inline uint64_t PropFlagsOf(const FProperty* p) { return At<uint64_t>(p, L.prop_flags); }
inline int32_t OffsetOf(const FProperty* p) { return At<int32_t>(p, L.prop_offset); }
inline uint8_t BoolByteOffset(const FProperty* p) { return At<uint8_t>(p, L.boolprop_base + 1); }
inline uint8_t BoolFieldMask(const FProperty* p) { return At<uint8_t>(p, L.boolprop_base + 3); }
inline UEnum* ByteEnum(const FProperty* p) { return At<UEnum*>(p, L.byteprop_enum); }
inline UEnum* EnumPropEnum(const FProperty* p) { return At<UEnum*>(p, L.enumprop_base + 8); }
inline UStruct* StructOf(const FProperty* p) { return At<UStruct*>(p, L.structprop_struct); }
inline FProperty* InnerOf(const FProperty* p) { return At<FProperty*>(p, L.arrayprop_inner); }
inline UClass* ObjClassOf(const FProperty* p) { return At<UClass*>(p, L.objprop_class); }
inline UClass* MetaClassOf(const FProperty* p) { return At<UClass*>(p, L.classprop_meta); }
inline FProperty* MapKeyOf(const FProperty* p) { return At<FProperty*>(p, L.mapprop_base); }
inline FProperty* MapValueOf(const FProperty* p) { return At<FProperty*>(p, L.mapprop_base + 8); }
inline FProperty* SetElemOf(const FProperty* p) { return At<FProperty*>(p, L.setprop_elem); }

// ---- names and walking -----------------------------------------------------------------------------
std::string Str(const FName& n);                                    // "Name" or "Name_<n>" (like the engine prints it)
// Same text as the SDK's GetName(): the base name WITHOUT the instance number ("ScaleBox", not "ScaleBox_2"). Mods and the
// built-in scripts look objects up by these names. Str() (with the number) is for FName property VALUES only.
inline std::string NameStr(const UObject* o) { return NameOf(o).ToString(); }
inline std::string NameStr(const FField* f) { return NameOf(f).ToString(); }
inline std::string NameStr(const FFieldClass* c) { return NameOf(c).ToString(); }
std::string FullName(const UObject* o);                             // "Class Outer.Name" without package paths (like UObject::GetFullName)
std::string PathName(const UObject* o);                             // package-qualified path
bool IsA(const UObject* o, const UClass* cls);                      // o's class is cls or derives from it
bool IsCdo(const UObject* o);
UClass* FindClass(const std::string& name);                         // by short name ("Actor", "WBP_X_C"), cached
UClass* ClassClass();                                               // the UClass called "Class"
UClass* FunctionClass();                                            // "Function"
FProperty* FindProp(const UStruct* s, const std::string& name);     // searches the super chain
UFunction* FindFunc(const UStruct* s, const std::string& name);
UObject* DefaultObject(const std::string& className);               // the class default object by class name

// ---- calling UFunctions by name from C++ -----------------------------------------------------------
// Args a(UFunction*) ; a.Set("Name", value) ; a.Call(context) ; a.Get<T>("ReturnValue")
class Args
{
public:
    explicit Args(UFunction* fn);
    template <class T> Args& Set(const char* name, const T& v) { int o = Find(name, sizeof(T)); if (o >= 0) memcpy(&buf_[o], &v, sizeof(T)); return *this; }
    template <class T> T Get(const char* name) { T v{}; int o = Find(name, sizeof(T)); if (o >= 0) memcpy(&v, &buf_[o], sizeof(T)); return v; }
    bool Call(UObject* context);                                     // via ProcessEvent (the vtable slot, independent of the layouts)
    bool ok() const { return fn_ != nullptr; }
private:
    int Find(const char* name, size_t size);
    UFunction* fn_;
    std::vector<uint8_t> buf_;
};
// Args for "Class.Function"; call it on the class default object of Class (static library functions) or on any instance.
Args Prepare(const char* cls, const char* func);
UObject* CdoFor(const char* cls);
UObject* FindFirst(const char* cls);                                // first live (non-default) instance, scans all objects
UObject* ObjProp(const UObject* o, const char* prop);               // reads an object-valued property by name (nullptr if none)
}
