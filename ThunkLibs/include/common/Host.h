/*
$info$
category: thunklibs ~ These are generated + glue logic 1:1 thunks unless noted otherwise
$end_info$
*/

#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

#include "PackedArguments.h"

// Import FEXCore functions for use in host thunk libraries.
//
// Note these are statically linked into the FEX executable. The linker hence
// doesn't know about them when linking thunk libraries. This issue is avoided
// by declaring the functions as weak symbols.
namespace FEXCore {
  struct HostToGuestTrampolinePtr;

  __attribute__((weak))
  HostToGuestTrampolinePtr*
  MakeHostTrampolineForGuestFunction(void* HostPacker, uintptr_t GuestTarget, uintptr_t GuestUnpacker);

  __attribute__((weak))
  HostToGuestTrampolinePtr*
  FinalizeHostTrampolineForGuestFunction(HostToGuestTrampolinePtr*, void* HostPacker);
}

template<typename Fn>
struct function_traits;
template<typename Result, typename Arg>
struct function_traits<Result(*)(Arg)> {
    using result_t = Result;
    using arg_t = Arg;
};

template<auto Fn>
static typename function_traits<decltype(Fn)>::result_t
fexfn_type_erased_unpack(void* argsv) {
    using args_t = typename function_traits<decltype(Fn)>::arg_t;
    return Fn(reinterpret_cast<args_t>(argsv));
}

struct ExportEntry { uint8_t* sha256; void(*fn)(void *); };

typedef void fex_call_callback_t(uintptr_t callback, void *arg0, void* arg1);

#define EXPORTS(name) \
  extern "C" { \
    ExportEntry* fexthunks_exports_##name() { \
      if (!fexldr_init_##name()) { \
        return nullptr; \
      } \
      return exports; \
    } \
  }

#define LOAD_LIB_INIT(init_fn) \
  __attribute__((constructor)) static void loadlib() \
  { \
    init_fn (); \
  }

struct GuestcallInfo {
  uintptr_t HostPacker;
  void (*CallCallback)(uintptr_t GuestUnpacker, uintptr_t GuestTarget, void* argsrv);
  uintptr_t GuestUnpacker;
  uintptr_t GuestTarget;
};

// Helper macro for reading an internal argument passed through the `r11`
// host register. This macro must be placed at the very beginning of
// the function it is used in.
#if defined(_M_X86_64)
#define LOAD_INTERNAL_GUESTPTR_VIA_CUSTOM_ABI(target_variable) \
  asm volatile("mov %%r11, %0" : "=r" (target_variable))
#elif defined(_M_ARM_64)
#define LOAD_INTERNAL_GUESTPTR_VIA_CUSTOM_ABI(target_variable) \
  asm volatile("mov %0, x11" : "=r" (target_variable))
#endif

struct ParameterAnnotations {
    bool is_passthrough = false;
    bool assume_compatible = false;
};

// Placeholder type to indicate the given data is in guest-layout
template<typename T>
struct guest_layout {
  static_assert(!std::is_class_v<T>, "No guest layout defined for this non-opaque struct type. This may be a bug in the thunk generator.");
  static_assert(!std::is_union_v<T>, "No guest layout defined for this non-opaque union type. This may be a bug in the thunk generator.");
  static_assert(!std::is_enum_v<T>, "No guest layout defined for this enum type. This is a bug in the thunk generator.");
  static_assert(!std::is_void_v<T>, "Attempted to get guest layout of void. Missing annotation for void pointer?");

  using type = std::enable_if_t<!std::is_pointer_v<T>, T>;
  type data;

  guest_layout& operator=(const T from) {
    data = from;
    return *this;
  }
};

template<typename T>
struct guest_layout<T*> {
#ifdef IS_32BIT_THUNK
  using type = uint32_t;
#else
  using type = uint64_t;
#endif
  type data;

  // Allow implicit conversion for function pointers, since they disallow use of host_layout
  guest_layout& operator=(const T* from) requires (std::is_function_v<T>) {
    // TODO: Assert upper 32 bits are zero
    data = reinterpret_cast<uintptr_t>(from);
    return *this;
  }

  guest_layout<T>* get_pointer() {
    return reinterpret_cast<guest_layout<T>*>(uintptr_t { data });
  }

  const guest_layout<T>* get_pointer() const {
    return reinterpret_cast<const guest_layout<T>*>(uintptr_t { data });
  }
};

template<typename T>
struct guest_layout<T* const> {
#ifdef IS_32BIT_THUNK
  using type = uint32_t;
#else
  using type = uint64_t;
#endif
  type data;

  // Allow implicit conversion for function pointers, since they disallow use of host_layout
  guest_layout& operator=(const T* from) requires (std::is_function_v<T>) {
    // TODO: Assert upper 32 bits are zero
    data = reinterpret_cast<uintptr_t>(from);
    return *this;
  }

  guest_layout<T>* get_pointer() {
    return reinterpret_cast<guest_layout<T>*>(uintptr_t { data });
  }

  const guest_layout<T>* get_pointer() const {
    return reinterpret_cast<const guest_layout<T>*>(uintptr_t { data });
  }
};

template<typename T>
struct host_layout;

template<typename T>
struct host_layout {
  static_assert(!std::is_class_v<T>, "No host_layout specialization generated for struct/class type");
  static_assert(!std::is_union_v<T>, "No host_layout specialization generated for union type");
  static_assert(!std::is_void_v<T>, "Attempted to get host layout of void. Missing annotation for void pointer?");

  // TODO: This generic implementation shouldn't be needed. Instead, auto-specialize host_layout for all types used as members.

  T data;

  explicit host_layout(const guest_layout<T>& from) requires (!std::is_enum_v<T>) : data { from.data } {
    // NOTE: This is not strictly neccessary since differently sized types may
    //       be used across architectures. It's important that the host type
    //       can represent all guest values without loss, however.
    static_assert(sizeof(data) == sizeof(from));
  }

  explicit host_layout(const guest_layout<T>& from) requires (std::is_enum_v<T>) : data { static_cast<T>(from.data) } {
  }
};

// Explicitly turn a host type into its corresponding host_layout
template<typename T>
const host_layout<T>& to_host_layout(const T& t) {
  static_assert(std::is_same_v<decltype(host_layout<T>::data), T>);
  return reinterpret_cast<const host_layout<T>&>(t);
}

template<typename T>
struct host_layout<T*> {
  T* data;

  static_assert(!std::is_function_v<T>, "Function types must be handled separately");

  // Assume underlying data is compatible and just convert the guest-sized pointer to 64-bit
  explicit host_layout(const guest_layout<T*>& from) : data { (T*)(uintptr_t)from.data } {
  }

  // TODO: Make this explicit?
  host_layout() = default;
};

template<typename T>
struct host_layout<T* const> {
  T* data;

  static_assert(!std::is_function_v<T>, "Function types must be handled separately");

  // Assume underlying data is compatible and just convert the guest-sized pointer to 64-bit
  explicit host_layout(const guest_layout<T* const>& from) : data { (T*)(uintptr_t)from.data } {
  }
};

template<typename T>
inline guest_layout<T> to_guest(const host_layout<T>& from) requires(!std::is_pointer_v<T>) {
  if constexpr (std::is_enum_v<T>) {
    // enums are represented by fixed-size integers in guest_layout, so explicitly cast them
    return guest_layout<T> { static_cast<std::underlying_type_t<T>>(from.data) };
  } else {
    guest_layout<T> ret { .data = from.data };
    return ret;
  }
}

template<typename T>
inline guest_layout<T*> to_guest(const host_layout<T*>& from) {
  // TODO: Assert upper 32 bits are zero
  guest_layout<T*> ret;
  ret.data = reinterpret_cast<uintptr_t>(from.data);
  return ret;
}

template<typename>
struct CallbackUnpack;

template<typename Result, typename... Args>
struct CallbackUnpack<Result(Args...)> {
  static Result CallGuestPtr(Args... args) {
    GuestcallInfo *guestcall;
    LOAD_INTERNAL_GUESTPTR_VIA_CUSTOM_ABI(guestcall);

    PackedArguments<Result, Args...> packed_args = {
      args...
    };
    guestcall->CallCallback(guestcall->GuestUnpacker, guestcall->GuestTarget, &packed_args);
    if constexpr (!std::is_void_v<Result>) {
      return packed_args.rv;
    }
  }
};

template<ParameterAnnotations Annotation, typename T>
auto Projection(guest_layout<T>& data) {
  if constexpr (Annotation.is_passthrough) {
    return data;
  } else {
    return host_layout<T> { data }.data;
  }
}

template<typename>
struct GuestWrapperForHostFunction;

template<typename Result, typename... Args>
struct GuestWrapperForHostFunction<Result(Args...)> {
  // Host functions called from Guest
  template<ParameterAnnotations... Annotations>
  static void Call(void* argsv) {
    static_assert(sizeof...(Annotations) == sizeof...(Args));

    auto args = reinterpret_cast<PackedArguments<Result, guest_layout<Args>..., uintptr_t>*>(argsv);
    constexpr auto CBIndex = sizeof...(Args);
    uintptr_t cb;
    static_assert(CBIndex <= 18 || CBIndex == 23);
    if constexpr(CBIndex == 0) {
      cb = args->a0;
    } else if constexpr(CBIndex == 1) {
      cb = args->a1;
    } else if constexpr(CBIndex == 2) {
      cb = args->a2;
    } else if constexpr(CBIndex == 3) {
      cb = args->a3;
    } else if constexpr(CBIndex == 4) {
      cb = args->a4;
    } else if constexpr(CBIndex == 5) {
      cb = args->a5;
    } else if constexpr(CBIndex == 6) {
      cb = args->a6;
    } else if constexpr(CBIndex == 7) {
      cb = args->a7;
    } else if constexpr(CBIndex == 8) {
      cb = args->a8;
    } else if constexpr(CBIndex == 9) {
      cb = args->a9;
    } else if constexpr(CBIndex == 10) {
      cb = args->a10;
    } else if constexpr(CBIndex == 11) {
      cb = args->a11;
    } else if constexpr(CBIndex == 12) {
      cb = args->a12;
    } else if constexpr(CBIndex == 13) {
      cb = args->a13;
    } else if constexpr(CBIndex == 14) {
      cb = args->a14;
    } else if constexpr(CBIndex == 15) {
      cb = args->a15;
    } else if constexpr(CBIndex == 16) {
      cb = args->a16;
    } else if constexpr(CBIndex == 17) {
      cb = args->a17;
    } else if constexpr(CBIndex == 18) {
      cb = args->a18;
    } else if constexpr(CBIndex == 23) {
      cb = args->a23;
    }

    // This is almost the same type as "Result func(Args..., uintptr_t)", but
    // individual parameters annotated as passthrough are replaced by guest_layout<GuestArgs>
    auto callback = reinterpret_cast<Result(*)(std::conditional_t<Annotations.is_passthrough, guest_layout<Args>, Args>..., uintptr_t)>(cb);

    auto f = [&callback](guest_layout<Args>... args, uintptr_t target) -> Result {
      // Fold over each of Annotations, Args, and args. This will match up the elements in triplets.
      return callback(Projection<Annotations, Args>(args)..., target);
    };
    Invoke(f, *args);
  }
};

template<typename FuncType>
void MakeHostTrampolineForGuestFunctionAt(uintptr_t GuestTarget, uintptr_t GuestUnpacker, FuncType **Func) {
    *Func = (FuncType*)FEXCore::MakeHostTrampolineForGuestFunction(
        (void*)&CallbackUnpack<FuncType>::CallGuestPtr,
        GuestTarget,
        GuestUnpacker);
}

template<typename F>
void FinalizeHostTrampolineForGuestFunction(F* PreallocatedTrampolineForGuestFunction) {
  FEXCore::FinalizeHostTrampolineForGuestFunction(
      (FEXCore::HostToGuestTrampolinePtr*)PreallocatedTrampolineForGuestFunction,
      (void*)&CallbackUnpack<F>::CallGuestPtr);
}

template<typename F>
void FinalizeHostTrampolineForGuestFunction(guest_layout<F*> PreallocatedTrampolineForGuestFunction) {
  FEXCore::FinalizeHostTrampolineForGuestFunction(
      (FEXCore::HostToGuestTrampolinePtr*)PreallocatedTrampolineForGuestFunction.data,
      (void*)&CallbackUnpack<F>::CallGuestPtr);
}

// In the case of the thunk host_loader being the default, FEX need to use dlsym with RTLD_DEFAULT.
// If FEX queried the symbol object directly then it wouldn't follow symbol overriding rules.
//
// Common usecase is LD_PRELOAD with a library that defines some symbols.
// And then programs and libraries will pick up the preloaded symbols.
// ex: MangoHud overrides GLX and EGL symbols.
inline
void *dlsym_default(void* handle, const char* symbol) {
  return dlsym(RTLD_DEFAULT, symbol);
}
