/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineIC_h
#define jit_BaselineIC_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "gc/Barrier.h"
#include "gc/GC.h"
#include "gc/Rooting.h"
#include "jit/BaselineICList.h"
#include "jit/ICState.h"
#include "jit/ICStubSpace.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/SharedICRegisters.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/ArrayObject.h"
#include "vm/JSScript.h"

class JS_PUBLIC_API JSTracer;

namespace js {

MOZ_COLD void ReportOutOfMemory(JSContext* cx);

namespace jit {

class BaselineFrame;
class CacheIRStubInfo;
class ICScript;
class MacroAssembler;

enum class TailCallVMFunctionId;
enum class VMFunctionId;

// [SMDOC] JIT Inline Caches (ICs)
//
// Baseline Inline Caches are polymorphic caches that aggressively
// share their stub code.
//
// Every polymorphic site contains a linked list of stubs which are
// specific to that site.  These stubs are composed of a |StubData|
// structure that stores parametrization information (e.g.
// the shape pointer for a shape-check-and-property-get stub), any
// dynamic information (e.g. warm-up counters), a pointer to the stub code,
// and a pointer to the next stub state in the linked list.
//
// Every BaselineScript keeps an table of |CacheDescriptor| data
// structures, which store the following:
//      A pointer to the first StubData in the cache.
//      The bytecode PC of the relevant IC.
//      The machine-code PC where the call to the stubcode returns.
//
// A diagram:
//
//        Control flow                  Pointers
//      =======#                     ----.     .---->
//             #                         |     |
//             #======>                  \-----/
//
//
//                                   .---------------------------------------.
//                                   |         .-------------------------.   |
//                                   |         |         .----.          |   |
//         Baseline                  |         |         |    |          |   |
//         JIT Code              0   ^     1   ^     2   ^    |          |   |
//     +--------------+    .-->+-----+   +-----+   +-----+    |          |   |
//     |              |  #=|==>|     |==>|     |==>| FB  |    |          |   |
//     |              |  # |   +-----+   +-----+   +-----+    |          |   |
//     |              |  # |      #         #         #       |          |   |
//     |==============|==# |      #         #         #       |          |   |
//     |=== IC =======|    |      #         #         #       |          |   |
//  .->|==============|<===|======#=========#=========#       |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  |          |   |
//  |  |              |    |                                  v          |   |
//  |  |              |    |                              +---------+    |   |
//  |  |              |    |                              | Fallback|    |   |
//  |  |              |    |                              | Stub    |    |   |
//  |  |              |    |                              | Code    |    |   |
//  |  |              |    |                              +---------+    |   |
//  |  +--------------+    |                                             |   |
//  |         |_______     |                              +---------+    |   |
//  |                |     |                              | Stub    |<---/   |
//  |        IC      |     \--.                           | Code    |        |
//  |    Descriptor  |        |                           +---------+        |
//  |      Table     v        |                                              |
//  |  +-----------------+    |                           +---------+        |
//  \--| Ins | PC | Stub |----/                           | Stub    |<-------/
//     +-----------------+                                | Code    |
//     |       ...       |                                +---------+
//     +-----------------+
//                                                          Shared
//                                                          Stub Code
//

class ICStub;
class ICFallbackStub;

#define FORWARD_DECLARE_STUBS(kindName) class IC##kindName;
IC_BASELINE_STUB_KIND_LIST(FORWARD_DECLARE_STUBS)
#undef FORWARD_DECLARE_STUBS

#ifdef JS_JITSPEW
void FallbackICSpew(JSContext* cx, ICFallbackStub* stub, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(3, 4);
#else
#  define FallbackICSpew(...)
#endif

// An entry in the BaselineScript IC descriptor table. There's one ICEntry per
// IC.
class ICEntry {
  // A pointer to the first IC stub for this instruction.
  ICStub* firstStub_;

  // The PC offset of this IC's bytecode op within the JSScript.
  uint32_t pcOffset_;

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
#  ifdef JS_64BIT
  // On 64-bit architectures, we have 32 bits of alignment padding.
  // We fill it with a magic value, and check that value when tracing.
  static const uint32_t EXPECTED_TRACE_MAGIC = 0xdeaddead;
  uint32_t traceMagic_ = EXPECTED_TRACE_MAGIC;
#  endif
#endif

 public:
  ICEntry(ICStub* firstStub, uint32_t pcOffset)
      : firstStub_(firstStub), pcOffset_(pcOffset) {}

  ICStub* firstStub() const {
    MOZ_ASSERT(firstStub_);
    return firstStub_;
  }

  ICFallbackStub* fallbackStub() const;

  void setFirstStub(ICStub* stub) { firstStub_ = stub; }

  uint32_t pcOffset() const { return pcOffset_; }
  jsbytecode* pc(JSScript* script) const {
    return script->offsetToPC(pcOffset());
  }

  static constexpr size_t offsetOfFirstStub() {
    return offsetof(ICEntry, firstStub_);
  }

  inline ICStub** addressOfFirstStub() { return &firstStub_; }

  void trace(JSTracer* trc);
};

// Constant iterator that traverses arbitrary chains of ICStubs.
// No requirements are made of the ICStub used to construct this
// iterator, aside from that the stub be part of a nullptr-terminated
// chain.
// The iterator is considered to be at its end once it has been
// incremented _past_ the last stub.  Thus, if 'atEnd()' returns
// true, the '*' and '->' operations are not valid.
class ICStubConstIterator {
  friend class ICStub;
  friend class ICFallbackStub;

 private:
  ICStub* currentStub_;

 public:
  explicit ICStubConstIterator(ICStub* currentStub)
      : currentStub_(currentStub) {}

  static ICStubConstIterator StartingAt(ICStub* stub) {
    return ICStubConstIterator(stub);
  }
  static ICStubConstIterator End(ICStub* stub) {
    return ICStubConstIterator(nullptr);
  }

  bool operator==(const ICStubConstIterator& other) const {
    return currentStub_ == other.currentStub_;
  }
  bool operator!=(const ICStubConstIterator& other) const {
    return !(*this == other);
  }

  ICStubConstIterator& operator++();

  ICStubConstIterator operator++(int) {
    ICStubConstIterator oldThis(*this);
    ++(*this);
    return oldThis;
  }

  ICStub* operator*() const {
    MOZ_ASSERT(currentStub_);
    return currentStub_;
  }

  ICStub* operator->() const {
    MOZ_ASSERT(currentStub_);
    return currentStub_;
  }

  bool atEnd() const { return currentStub_ == nullptr; }
};

// Iterator that traverses "regular" IC chains that start at an ICEntry
// and are terminated with an ICFallbackStub.
//
// The iterator is considered to be at its end once it is _at_ the
// fallback stub.  Thus, unlike the ICStubConstIterator, operators
// '*' and '->' are valid even if 'atEnd()' returns true - they
// will act on the fallback stub.
//
// This iterator also allows unlinking of stubs being traversed.
// Note that 'unlink' does not implicitly advance the iterator -
// it must be advanced explicitly using '++'.
class ICStubIterator {
  friend class ICFallbackStub;

 private:
  ICEntry* icEntry_;
  ICFallbackStub* fallbackStub_;
  ICStub* previousStub_;
  ICStub* currentStub_;
  bool unlinked_;

  explicit ICStubIterator(ICFallbackStub* fallbackStub, bool end = false);

 public:
  bool operator==(const ICStubIterator& other) const {
    // == should only ever be called on stubs from the same chain.
    MOZ_ASSERT(icEntry_ == other.icEntry_);
    MOZ_ASSERT(fallbackStub_ == other.fallbackStub_);
    return currentStub_ == other.currentStub_;
  }
  bool operator!=(const ICStubIterator& other) const {
    return !(*this == other);
  }

  ICStubIterator& operator++();

  ICStubIterator operator++(int) {
    ICStubIterator oldThis(*this);
    ++(*this);
    return oldThis;
  }

  ICStub* operator*() const { return currentStub_; }

  ICStub* operator->() const { return currentStub_; }

  bool atEnd() const { return currentStub_ == (ICStub*)fallbackStub_; }

  void unlink(JSContext* cx, JSScript* script);
};

//
// Base class for all IC stubs.
//
class ICStub {
  friend class ICFallbackStub;

 public:
  enum Kind : uint16_t {
    INVALID = 0,
#define DEF_ENUM_KIND(kindName) kindName,
    IC_BASELINE_STUB_KIND_LIST(DEF_ENUM_KIND)
#undef DEF_ENUM_KIND
        LIMIT
  };

  static bool IsValidKind(Kind k) { return (k > INVALID) && (k < LIMIT); }
  static bool IsCacheIRKind(Kind k) { return k == CacheIR_Regular; }

  static const char* KindString(Kind k) {
    switch (k) {
#define DEF_KIND_STR(kindName) \
  case kindName:               \
    return #kindName;
      IC_BASELINE_STUB_KIND_LIST(DEF_KIND_STR)
#undef DEF_KIND_STR
      default:
        MOZ_CRASH("Invalid kind.");
    }
  }

  // TODO(no-TI): remove enum
  enum Trait : uint16_t {
    Regular = 0x0,
    Fallback = 0x1,
  };

  void updateCode(JitCode* stubCode);
  void trace(JSTracer* trc);

  template <typename T, typename... Args>
  static T* New(JSContext* cx, ICStubSpace* space, JitCode* code,
                Args&&... args) {
    if (!code) {
      return nullptr;
    }
    T* result = space->allocate<T>(code, std::forward<Args>(args)...);
    if (!result) {
      ReportOutOfMemory(cx);
    }
    return result;
  }

  template <typename T, typename... Args>
  static T* NewFallback(JSContext* cx, ICStubSpace* space, TrampolinePtr code,
                        Args&&... args) {
    T* result = space->allocate<T>(code, std::forward<Args>(args)...);
    if (MOZ_UNLIKELY(!result)) {
      ReportOutOfMemory(cx);
    }
    return result;
  }

 protected:
  // The raw jitcode to call for this stub.
  uint8_t* stubCode_;

  // Pointer to next IC stub.  This is null for the last IC stub, which should
  // either be a fallback or inert IC stub.
  ICStub* next_ = nullptr;

  // A 16-bit field usable by subtypes of ICStub for subtype-specific small-info
  uint16_t extra_ = 0;

  // A 16-bit field storing the trait and kind.
  // Unused bits are filled with a magic value and verified when tracing.
  uint16_t traitKindBits_;

  static const uint16_t TRAIT_OFFSET = 0;
  static const uint16_t TRAIT_BITS = 3;
  static const uint16_t TRAIT_MASK = (1 << TRAIT_BITS) - 1;
  static const uint16_t KIND_OFFSET = TRAIT_OFFSET + TRAIT_BITS;
  static const uint16_t KIND_BITS = 5;
  static const uint16_t KIND_MASK = (1 << KIND_BITS) - 1;
  static const uint16_t MAGIC_OFFSET = KIND_OFFSET + KIND_BITS;
  static const uint16_t MAGIC_BITS = 8;
  static const uint16_t MAGIC_MASK = (1 << MAGIC_BITS) - 1;
  static const uint16_t EXPECTED_MAGIC = 0b11100011;

  static_assert(LIMIT <= (1 << KIND_BITS), "Not enough kind bits");
  static_assert(LIMIT > (1 << (KIND_BITS - 1)), "Too many kind bits");
  static_assert(TRAIT_BITS + KIND_BITS + MAGIC_BITS == 16, "Unused bits");

  inline ICStub(Kind kind, uint8_t* stubCode) : stubCode_(stubCode) {
    setTraitKind(Regular, kind);
    MOZ_ASSERT(stubCode != nullptr);
  }

  inline ICStub(Kind kind, JitCode* stubCode) : ICStub(kind, stubCode->raw()) {
    MOZ_ASSERT(stubCode != nullptr);
  }

  inline ICStub(Kind kind, Trait trait, uint8_t* stubCode)
      : stubCode_(stubCode) {
    setTraitKind(trait, kind);
    MOZ_ASSERT(stubCode != nullptr);
  }

  inline ICStub(Kind kind, Trait trait, JitCode* stubCode)
      : ICStub(kind, trait, stubCode->raw()) {
    MOZ_ASSERT(stubCode != nullptr);
  }

  inline Trait trait() const {
    return (Trait)((traitKindBits_ >> TRAIT_OFFSET) & TRAIT_MASK);
  }

  inline void setTraitKind(Trait trait, Kind kind) {
    traitKindBits_ = (trait << TRAIT_OFFSET) | (kind << KIND_OFFSET) |
                     (EXPECTED_MAGIC << MAGIC_OFFSET);
  }

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  inline void checkTraceMagic() {
    uint16_t magic = (traitKindBits_ >> MAGIC_OFFSET) & MAGIC_MASK;
    MOZ_DIAGNOSTIC_ASSERT(magic == EXPECTED_MAGIC);
  }
#endif

 public:
  inline Kind kind() const {
    return (Kind)((traitKindBits_ >> KIND_OFFSET) & KIND_MASK);
  }

  inline bool isFallback() const { return trait() == Fallback; }

  inline const ICFallbackStub* toFallbackStub() const {
    MOZ_ASSERT(isFallback());
    return reinterpret_cast<const ICFallbackStub*>(this);
  }

  inline ICFallbackStub* toFallbackStub() {
    MOZ_ASSERT(isFallback());
    return reinterpret_cast<ICFallbackStub*>(this);
  }

#define KIND_METHODS(kindName)                                    \
  inline bool is##kindName() const { return kind() == kindName; } \
  inline const IC##kindName* to##kindName() const {               \
    MOZ_ASSERT(is##kindName());                                   \
    return reinterpret_cast<const IC##kindName*>(this);           \
  }                                                               \
  inline IC##kindName* to##kindName() {                           \
    MOZ_ASSERT(is##kindName());                                   \
    return reinterpret_cast<IC##kindName*>(this);                 \
  }
  IC_BASELINE_STUB_KIND_LIST(KIND_METHODS)
#undef KIND_METHODS

  inline ICStub* next() const { return next_; }

  inline bool hasNext() const { return next_ != nullptr; }

  inline void setNext(ICStub* stub) { next_ = stub; }

  inline ICStub** addressOfNext() { return &next_; }

  bool usesTrampolineCode() const {
    // All fallback code is stored in a single JitCode instance, so we can't
    // call JitCode::FromExecutable on the raw pointer.
    return isFallback();
  }
  JitCode* jitCode() {
    MOZ_ASSERT(!usesTrampolineCode());
    return JitCode::FromExecutable(stubCode_);
  }

  inline uint8_t* rawStubCode() const { return stubCode_; }

  inline ICFallbackStub* getChainFallback() {
    ICStub* lastStub = this;
    while (lastStub->next_) {
      lastStub = lastStub->next_;
    }
    MOZ_ASSERT(lastStub->isFallback());
    return lastStub->toFallbackStub();
  }

  inline ICStubConstIterator beginHere() {
    return ICStubConstIterator::StartingAt(this);
  }

  static inline size_t offsetOfNext() { return offsetof(ICStub, next_); }

  static inline size_t offsetOfStubCode() {
    return offsetof(ICStub, stubCode_);
  }

  static inline size_t offsetOfExtra() { return offsetof(ICStub, extra_); }

  static bool NonCacheIRStubMakesGCCalls(Kind kind);
  bool makesGCCalls() const;

  // Returns the number of times this stub has been entered. Must only be called
  // on stubs that have an enteredCount_ field (CacheIR or fallback stubs).
  uint32_t getEnteredCount() const;

  // Optimized stubs get purged on GC.  But some stubs can be active on the
  // stack during GC - specifically the ones that can make calls.  To ensure
  // that these do not get purged, all stubs that can make calls are allocated
  // in the fallback stub space.
  bool allocatedInFallbackSpace() const {
    MOZ_ASSERT(next());
    return makesGCCalls();
  }

  const CacheIRStubInfo* cacheIRStubInfo() const;
  const uint8_t* cacheIRStubData();
};

class ICFallbackStub : public ICStub {
  friend class ICStubConstIterator;

 protected:
  // Fallback stubs need these fields to easily add new stubs to
  // the linked list of stubs for an IC.

  // The IC entry in JitScript for this linked list of stubs.
  ICEntry* icEntry_ = nullptr;

  // The state of this IC
  ICState state_{};

  // Counts the number of times the stub was entered
  //
  // See Bug 1494473 comment 6 for a mechanism to handle overflow if overflow
  // becomes a concern.
  uint32_t enteredCount_ = 0;

  ICFallbackStub(Kind kind, TrampolinePtr stubCode)
      : ICStub(kind, ICStub::Fallback, stubCode.value) {}

  ICFallbackStub(Kind kind, Trait trait, TrampolinePtr stubCode)
      : ICStub(kind, trait, stubCode.value) {
    MOZ_ASSERT(trait == ICStub::Fallback);
  }

 public:
  inline ICEntry* icEntry() const { return icEntry_; }

  inline size_t numOptimizedStubs() const { return state_.numOptimizedStubs(); }

  bool newStubIsFirstStub() const {
    return (state_.mode() == ICState::Mode::Specialized &&
            numOptimizedStubs() == 0);
  }

  ICState& state() { return state_; }

  // The icEntry_ field can't be initialized when the stub is created since we
  // won't know the ICEntry address until we add the stub to JitScript. This
  // method allows this field to be fixed up at that point.
  void fixupICEntry(ICEntry* icEntry) {
    MOZ_ASSERT(icEntry_ == nullptr);
    icEntry_ = icEntry;
  }

  // Add a new stub to the IC chain terminated by this fallback stub.
  void addNewStub(ICStub* stub) {
    MOZ_ASSERT(stub->next() == nullptr);
    stub->setNext(icEntry_->firstStub());
    icEntry_->setFirstStub(stub);
    state_.trackAttached();
  }

  ICStubConstIterator beginChainConst() const {
    return ICStubConstIterator(icEntry_->firstStub());
  }

  ICStubIterator beginChain() { return ICStubIterator(this); }

  void discardStubs(JSContext* cx, JSScript* script);

  void clearUsedByTranspiler() { state_.clearUsedByTranspiler(); }
  void setUsedByTranspiler() { state_.setUsedByTranspiler(); }

  TrialInliningState trialInliningState() const {
    return state_.trialInliningState();
  }
  void setTrialInliningState(TrialInliningState state) {
    state_.setTrialInliningState(state);
  }

  void trackNotAttached(JSContext* cx, JSScript* script);

  // If the transpiler optimized based on this IC, invalidate the script's Warp
  // code.
  void maybeInvalidateWarp(JSContext* cx, JSScript* script);

  void unlinkStubDontInvalidateWarp(Zone* zone, ICStub* prev, ICStub* stub);

  // Return the number of times this stub has successfully provided a value to
  // the caller.
  uint32_t enteredCount() const { return enteredCount_; }
  inline void incrementEnteredCount() { enteredCount_++; }
  void resetEnteredCount() { enteredCount_ = 0; }
};

// Shared trait for all CacheIR stubs.
template <typename Base>
class ICCacheIR_Trait : public Base {
  // Flags stored in the uint16_t extra_ field in ICStub.
  static constexpr uint16_t PreliminaryObjectBit = 1 << 0;

 protected:
  const CacheIRStubInfo* stubInfo_;

  // Counts the number of times the stub was entered
  //
  // See Bug 1494473 comment 6 for a mechanism to handle overflow if overflow
  // becomes a concern.
  uint32_t enteredCount_ = 0;

 public:
  template <typename... Args>
  explicit ICCacheIR_Trait(const CacheIRStubInfo* stubInfo, Args&&... args)
      : Base(args...), stubInfo_(stubInfo) {}

  const CacheIRStubInfo* stubInfo() const { return stubInfo_; }
  uint8_t* stubDataStart();

  // Return the number of times this stub has successfully provided a value to
  // the caller.
  uint32_t enteredCount() const { return enteredCount_; }
  void resetEnteredCount() { enteredCount_ = 0; }

  void notePreliminaryObject() { this->extra_ |= PreliminaryObjectBit; }
  bool hasPreliminaryObject() const {
    return (this->extra_ & PreliminaryObjectBit) != 0;
  }

  static constexpr size_t offsetOfEnteredCount() {
    using T = ICCacheIR_Trait<Base>;
    return offsetof(T, enteredCount_);
  }
};

// Base class for Trait::Regular CacheIR stubs
// TODO(no-TI): remove trait class.
class ICCacheIR_Regular : public ICCacheIR_Trait<ICStub> {
  using Base = ICCacheIR_Trait<ICStub>;

 public:
  ICCacheIR_Regular(JitCode* stubCode, const CacheIRStubInfo* stubInfo)
      : Base(stubInfo, ICStub::CacheIR_Regular, stubCode) {}
};

// Base class for stubcode compilers.
class ICStubCompilerBase {
 protected:
  JSContext* cx;
  bool inStubFrame_ = false;

#ifdef DEBUG
  bool entersStubFrame_ = false;
  uint32_t framePushedAtEnterStubFrame_ = 0;
#endif

  explicit ICStubCompilerBase(JSContext* cx) : cx(cx) {}

  void pushCallArguments(MacroAssembler& masm,
                         AllocatableGeneralRegisterSet regs, Register argcReg,
                         bool isConstructing);

  // Push a payload specialized per compiler needed to execute stubs.
  void PushStubPayload(MacroAssembler& masm, Register scratch);
  void pushStubPayload(MacroAssembler& masm, Register scratch);

  // Emits a tail call to a VMFunction wrapper.
  MOZ_MUST_USE bool tailCallVMInternal(MacroAssembler& masm,
                                       TailCallVMFunctionId id);

  template <typename Fn, Fn fn>
  MOZ_MUST_USE bool tailCallVM(MacroAssembler& masm);

  // Emits a normal (non-tail) call to a VMFunction wrapper.
  MOZ_MUST_USE bool callVMInternal(MacroAssembler& masm, VMFunctionId id);

  template <typename Fn, Fn fn>
  MOZ_MUST_USE bool callVM(MacroAssembler& masm);

  // A stub frame is used when a stub wants to call into the VM without
  // performing a tail call. This is required for the return address
  // to pc mapping to work.
  void enterStubFrame(MacroAssembler& masm, Register scratch);
  void assumeStubFrame();
  void leaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false);

 public:
  static inline AllocatableGeneralRegisterSet availableGeneralRegs(
      size_t numInputs) {
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
#if defined(JS_CODEGEN_ARM)
    MOZ_ASSERT(!regs.has(BaselineStackReg));
    MOZ_ASSERT(!regs.has(ICTailCallReg));
    regs.take(BaselineSecondScratchReg);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    MOZ_ASSERT(!regs.has(BaselineStackReg));
    MOZ_ASSERT(!regs.has(ICTailCallReg));
    MOZ_ASSERT(!regs.has(BaselineSecondScratchReg));
#elif defined(JS_CODEGEN_ARM64)
    MOZ_ASSERT(!regs.has(PseudoStackPointer));
    MOZ_ASSERT(!regs.has(RealStackPointer));
    MOZ_ASSERT(!regs.has(ICTailCallReg));
#else
    MOZ_ASSERT(!regs.has(BaselineStackReg));
#endif
    regs.take(BaselineFrameReg);
    regs.take(ICStubReg);
#ifdef JS_CODEGEN_X64
    regs.take(ExtractTemp0);
    regs.take(ExtractTemp1);
#endif

    switch (numInputs) {
      case 0:
        break;
      case 1:
        regs.take(R0);
        break;
      case 2:
        regs.take(R0);
        regs.take(R1);
        break;
      default:
        MOZ_CRASH("Invalid numInputs");
    }

    return regs;
  }
};

// TODO(no-TI): remove/cleanup with ICStubCompilerBase.
class ICStubCompiler : public ICStubCompilerBase {
  // Prevent GC in the middle of stub compilation.
  js::gc::AutoSuppressGC suppressGC;

 protected:
  ICStub::Kind kind;

  // By default the stubcode key is just the kind.
  virtual int32_t getKey() const { return static_cast<int32_t>(kind); }

  virtual MOZ_MUST_USE bool generateStubCode(MacroAssembler& masm) = 0;

  ICStubCompiler(JSContext* cx, ICStub::Kind kind)
      : ICStubCompilerBase(cx), suppressGC(cx), kind(kind) {}

 protected:
  template <typename T, typename... Args>
  T* newStub(Args&&... args) {
    return ICStub::New<T>(cx, std::forward<Args>(args)...);
  }

 public:
  virtual ICStub* getStub(ICStubSpace* space) = 0;

  static ICStubSpace* StubSpaceForStub(bool makesGCCalls, JSScript* script,
                                       ICScript* icScript);
};

// ToBool
//      JSOp::IfNe

class ICToBool_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICToBool_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::ToBool_Fallback, stubCode) {}

 public:
  static const uint32_t MAX_OPTIMIZED_STUBS = 8;
};

// GetElem
//      JSOp::GetElem
//      JSOp::GetElemSuper

class ICGetElem_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICGetElem_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::GetElem_Fallback, stubCode) {}

  static const uint16_t EXTRA_NEGATIVE_INDEX = 0x1;
  static const uint16_t SAW_NON_INTEGER_INDEX_BIT = 0x2;

 public:
  void noteNegativeIndex() { extra_ |= EXTRA_NEGATIVE_INDEX; }
  bool hasNegativeIndex() const { return extra_ & EXTRA_NEGATIVE_INDEX; }

  void setSawNonIntegerIndex() { extra_ |= SAW_NON_INTEGER_INDEX_BIT; }
  bool sawNonIntegerIndex() const { return extra_ & SAW_NON_INTEGER_INDEX_BIT; }
};

// SetElem
//      JSOp::SetElem
//      JSOp::InitElem

class ICSetElem_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICSetElem_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::SetElem_Fallback, stubCode) {}

  static const size_t HasDenseAddFlag = 0x1;
  static const size_t HasTypedArrayOOBFlag = 0x2;

 public:
  void noteHasDenseAdd() { extra_ |= HasDenseAddFlag; }
  bool hasDenseAdd() const { return extra_ & HasDenseAddFlag; }

  void noteHasTypedArrayOOB() { extra_ |= HasTypedArrayOOBFlag; }
  bool hasTypedArrayOOB() const { return extra_ & HasTypedArrayOOBFlag; }
};

// In
//      JSOp::In
class ICIn_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICIn_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::In_Fallback, stubCode) {}
};

// HasOwn
//      JSOp::HasOwn
class ICHasOwn_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICHasOwn_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::HasOwn_Fallback, stubCode) {}
};

// CheckPrivateField
//      JSOp::CheckPrivateField
class ICCheckPrivateField_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICCheckPrivateField_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::CheckPrivateField_Fallback, stubCode) {}
};

// GetName
//      JSOp::GetName
//      JSOp::GetGName
class ICGetName_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICGetName_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::GetName_Fallback, stubCode) {}
};

// BindName
//      JSOp::BindName
class ICBindName_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICBindName_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::BindName_Fallback, stubCode) {}
};

// GetIntrinsic
//      JSOp::GetIntrinsic
class ICGetIntrinsic_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICGetIntrinsic_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::GetIntrinsic_Fallback, stubCode) {}
};

// GetProp
//     JSOp::GetProp
//     JSOp::GetPropSuper

class ICGetProp_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICGetProp_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::GetProp_Fallback, stubCode) {}

 public:
  // Whether this bytecode op called a getter. This is used by IonBuilder.
  // To improve performance, the flag is not set if WarpBuilder is enabled.
  static const size_t ACCESSED_GETTER_BIT = 1;

  void noteAccessedGetter() { extra_ |= (1u << ACCESSED_GETTER_BIT); }
  bool hasAccessedGetter() const {
    return extra_ & (1u << ACCESSED_GETTER_BIT);
  }
};

// SetProp
//     JSOp::SetProp
//     JSOp::SetName
//     JSOp::SetGName
//     JSOp::InitProp

class ICSetProp_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICSetProp_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::SetProp_Fallback, stubCode) {}
};

// Call
//      JSOp::Call
//      JSOp::CallIgnoresRv
//      JSOp::CallIter
//      JSOp::FunApply
//      JSOp::FunCall
//      JSOp::New
//      JSOp::SuperCall
//      JSOp::Eval
//      JSOp::StrictEval
//      JSOp::SpreadCall
//      JSOp::SpreadNew
//      JSOp::SpreadSuperCall
//      JSOp::SpreadEval
//      JSOp::SpreadStrictEval

class ICCall_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

 public:
  static const uint32_t MAX_OPTIMIZED_STUBS = 16;

 private:
  explicit ICCall_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::Call_Fallback, stubCode) {}
};

// IC for constructing an iterator from an input value.
class ICGetIterator_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICGetIterator_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::GetIterator_Fallback, stubCode) {}
};

class ICOptimizeSpreadCall_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICOptimizeSpreadCall_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::OptimizeSpreadCall_Fallback, stubCode) {}
};

// InstanceOf
//      JSOp::Instanceof
class ICInstanceOf_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICInstanceOf_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::InstanceOf_Fallback, stubCode) {}
};

// TypeOf
//      JSOp::Typeof
//      JSOp::TypeofExpr
class ICTypeOf_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICTypeOf_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::TypeOf_Fallback, stubCode) {}
};

class ICToPropertyKey_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICToPropertyKey_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::ToPropertyKey_Fallback, stubCode) {}
};

class ICRest_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  GCPtrArrayObject templateObject_;

  ICRest_Fallback(TrampolinePtr stubCode, ArrayObject* templateObject)
      : ICFallbackStub(ICStub::Rest_Fallback, stubCode),
        templateObject_(templateObject) {}

 public:
  static const uint32_t MAX_OPTIMIZED_STUBS = 8;

  GCPtrArrayObject& templateObject() { return templateObject_; }
};

// UnaryArith
//     JSOp::BitNot
//     JSOp::Pos
//     JSOp::Neg
//     JSOp::Inc
//     JSOp::Dec
//     JSOp::ToNumeric

class ICUnaryArith_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICUnaryArith_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(UnaryArith_Fallback, stubCode) {}

 public:
  bool sawDoubleResult() { return extra_; }
  void setSawDoubleResult() { extra_ = 1; }
};

// Compare
//      JSOp::Lt
//      JSOp::Le
//      JSOp::Gt
//      JSOp::Ge
//      JSOp::Eq
//      JSOp::Ne
//      JSOp::StrictEq
//      JSOp::StrictNe

class ICCompare_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICCompare_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::Compare_Fallback, stubCode) {}
};

// BinaryArith
//      JSOp::Add, JSOp::Sub, JSOp::Mul, JSOp::Div, JSOp::Mod, JSOp::Pow,
//      JSOp::BitAnd, JSOp::BitXor, JSOp::BitOr
//      JSOp::Lsh, JSOp::Rsh, JSOp::Ursh

class ICBinaryArith_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  explicit ICBinaryArith_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(BinaryArith_Fallback, stubCode) {}

  static const uint16_t SAW_DOUBLE_RESULT_BIT = 0x1;

 public:
  static const uint32_t MAX_OPTIMIZED_STUBS = 8;

  bool sawDoubleResult() const { return extra_ & SAW_DOUBLE_RESULT_BIT; }
  void setSawDoubleResult() { extra_ |= SAW_DOUBLE_RESULT_BIT; }
};

// JSOp::NewArray

class ICNewArray_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  GCPtrArrayObject templateObject_;

  // The group used for objects created here is always available, even if the
  // template object itself is not.
  GCPtrObjectGroup templateGroup_;

  ICNewArray_Fallback(TrampolinePtr stubCode, ObjectGroup* templateGroup)
      : ICFallbackStub(ICStub::NewArray_Fallback, stubCode),
        templateObject_(nullptr),
        templateGroup_(templateGroup) {}

 public:
  GCPtrArrayObject& templateObject() { return templateObject_; }

  void setTemplateObject(ArrayObject* obj) {
    MOZ_ASSERT(obj->group() == templateGroup());
    templateObject_ = obj;
  }

  GCPtrObjectGroup& templateGroup() { return templateGroup_; }

  void setTemplateGroup(ObjectGroup* group) {
    templateObject_ = nullptr;
    templateGroup_ = group;
  }
};

// JSOp::NewObject

class ICNewObject_Fallback : public ICFallbackStub {
  friend class ICStubSpace;

  GCPtrObject templateObject_;

  explicit ICNewObject_Fallback(TrampolinePtr stubCode)
      : ICFallbackStub(ICStub::NewObject_Fallback, stubCode),
        templateObject_(nullptr) {}

 public:
  GCPtrObject& templateObject() { return templateObject_; }

  void setTemplateObject(JSObject* obj) { templateObject_ = obj; }
};

struct IonOsrTempData;

extern bool DoCallFallback(JSContext* cx, BaselineFrame* frame,
                           ICCall_Fallback* stub, uint32_t argc, Value* vp,
                           MutableHandleValue res);

extern bool DoSpreadCallFallback(JSContext* cx, BaselineFrame* frame,
                                 ICCall_Fallback* stub, Value* vp,
                                 MutableHandleValue res);

extern bool DoToBoolFallback(JSContext* cx, BaselineFrame* frame,
                             ICToBool_Fallback* stub, HandleValue arg,
                             MutableHandleValue ret);

extern bool DoGetElemSuperFallback(JSContext* cx, BaselineFrame* frame,
                                   ICGetElem_Fallback* stub, HandleValue lhs,
                                   HandleValue rhs, HandleValue receiver,
                                   MutableHandleValue res);

extern bool DoGetElemFallback(JSContext* cx, BaselineFrame* frame,
                              ICGetElem_Fallback* stub, HandleValue lhs,
                              HandleValue rhs, MutableHandleValue res);

extern bool DoSetElemFallback(JSContext* cx, BaselineFrame* frame,
                              ICSetElem_Fallback* stub, Value* stack,
                              HandleValue objv, HandleValue index,
                              HandleValue rhs);

extern bool DoInFallback(JSContext* cx, BaselineFrame* frame,
                         ICIn_Fallback* stub, HandleValue key,
                         HandleValue objValue, MutableHandleValue res);

extern bool DoHasOwnFallback(JSContext* cx, BaselineFrame* frame,
                             ICHasOwn_Fallback* stub, HandleValue keyValue,
                             HandleValue objValue, MutableHandleValue res);

extern bool DoCheckPrivateFieldFallback(JSContext* cx, BaselineFrame* frame,
                                        ICCheckPrivateField_Fallback* stub,
                                        HandleValue objValue,
                                        HandleValue keyValue,
                                        MutableHandleValue res);

extern bool DoGetNameFallback(JSContext* cx, BaselineFrame* frame,
                              ICGetName_Fallback* stub, HandleObject envChain,
                              MutableHandleValue res);

extern bool DoBindNameFallback(JSContext* cx, BaselineFrame* frame,
                               ICBindName_Fallback* stub, HandleObject envChain,
                               MutableHandleValue res);

extern bool DoGetIntrinsicFallback(JSContext* cx, BaselineFrame* frame,
                                   ICGetIntrinsic_Fallback* stub,
                                   MutableHandleValue res);

extern bool DoGetPropFallback(JSContext* cx, BaselineFrame* frame,
                              ICGetProp_Fallback* stub, MutableHandleValue val,
                              MutableHandleValue res);

extern bool DoGetPropSuperFallback(JSContext* cx, BaselineFrame* frame,
                                   ICGetProp_Fallback* stub,
                                   HandleValue receiver, MutableHandleValue val,
                                   MutableHandleValue res);

extern bool DoSetPropFallback(JSContext* cx, BaselineFrame* frame,
                              ICSetProp_Fallback* stub, Value* stack,
                              HandleValue lhs, HandleValue rhs);

extern bool DoGetIteratorFallback(JSContext* cx, BaselineFrame* frame,
                                  ICGetIterator_Fallback* stub,
                                  HandleValue value, MutableHandleValue res);

extern bool DoOptimizeSpreadCallFallback(JSContext* cx, BaselineFrame* frame,
                                         ICOptimizeSpreadCall_Fallback* stub,
                                         HandleValue value,
                                         MutableHandleValue res);

extern bool DoInstanceOfFallback(JSContext* cx, BaselineFrame* frame,
                                 ICInstanceOf_Fallback* stub, HandleValue lhs,
                                 HandleValue rhs, MutableHandleValue res);

extern bool DoTypeOfFallback(JSContext* cx, BaselineFrame* frame,
                             ICTypeOf_Fallback* stub, HandleValue val,
                             MutableHandleValue res);

extern bool DoToPropertyKeyFallback(JSContext* cx, BaselineFrame* frame,
                                    ICToPropertyKey_Fallback* stub,
                                    HandleValue val, MutableHandleValue res);

extern bool DoRestFallback(JSContext* cx, BaselineFrame* frame,
                           ICRest_Fallback* stub, MutableHandleValue res);

extern bool DoUnaryArithFallback(JSContext* cx, BaselineFrame* frame,
                                 ICUnaryArith_Fallback* stub, HandleValue val,
                                 MutableHandleValue res);

extern bool DoBinaryArithFallback(JSContext* cx, BaselineFrame* frame,
                                  ICBinaryArith_Fallback* stub, HandleValue lhs,
                                  HandleValue rhs, MutableHandleValue ret);

extern bool DoNewArrayFallback(JSContext* cx, BaselineFrame* frame,
                               ICNewArray_Fallback* stub, uint32_t length,
                               MutableHandleValue res);

extern bool DoNewObjectFallback(JSContext* cx, BaselineFrame* frame,
                                ICNewObject_Fallback* stub,
                                MutableHandleValue res);

extern bool DoCompareFallback(JSContext* cx, BaselineFrame* frame,
                              ICCompare_Fallback* stub, HandleValue lhs,
                              HandleValue rhs, MutableHandleValue ret);

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineIC_h */
