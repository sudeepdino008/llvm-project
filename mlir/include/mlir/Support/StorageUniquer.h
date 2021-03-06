//===- StorageUniquer.h - Common Storage Class Uniquer ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_SUPPORT_STORAGEUNIQUER_H
#define MLIR_SUPPORT_STORAGEUNIQUER_H

#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Allocator.h"

namespace mlir {
class TypeID;

namespace detail {
struct StorageUniquerImpl;

/// Trait to check if ImplTy provides a 'getKey' method with types 'Args'.
template <typename ImplTy, typename... Args>
using has_impltype_getkey_t = decltype(ImplTy::getKey(std::declval<Args>()...));

/// Trait to check if ImplTy provides a 'hashKey' method for 'T'.
template <typename ImplTy, typename T>
using has_impltype_hash_t = decltype(ImplTy::hashKey(std::declval<T>()));
} // namespace detail

/// A utility class to get, or create instances of storage classes. These
/// storage classes must respect the following constraints:
///    - Derive from StorageUniquer::BaseStorage.
///    - Provide an unsigned 'kind' value to be used as part of the unique'ing
///      process.
///
/// For non-parametric storage classes, i.e. those that are solely uniqued by
/// their kind, nothing else is needed. Instances of these classes can be
/// created by calling `get` without trailing arguments.
///
/// Otherwise, the parametric storage classes may be created with `get`,
/// and must respect the following:
///    - Define a type alias, KeyTy, to a type that uniquely identifies the
///      instance of the storage class within its kind.
///      * The key type must be constructible from the values passed into the
///        getComplex call after the kind.
///      * If the KeyTy does not have an llvm::DenseMapInfo specialization, the
///        storage class must define a hashing method:
///         'static unsigned hashKey(const KeyTy &)'
///
///    - Provide a method, 'bool operator==(const KeyTy &) const', to
///      compare the storage instance against an instance of the key type.
///
///    - Provide a static construction method:
///        'DerivedStorage *construct(StorageAllocator &, const KeyTy &key)'
///      that builds a unique instance of the derived storage. The arguments to
///      this function are an allocator to store any uniqued data and the key
///      type for this storage.
///
///    - Provide a cleanup method:
///        'void cleanup()'
///      that is called when erasing a storage instance. This should cleanup any
///      fields of the storage as necessary and not attempt to free the memory
///      of the storage itself.
///
/// Storage classes may have an optional mutable component, which must not take
/// part in the unique immutable key. In this case, storage classes may be
/// mutated with `mutate` and must additionally respect the following:
///    - Provide a mutation method:
///        'LogicalResult mutate(StorageAllocator &, <...>)'
///      that is called when mutating a storage instance. The first argument is
///      an allocator to store any mutable data, and the remaining arguments are
///      forwarded from the call site. The storage can be mutated at any time
///      after creation. Care must be taken to avoid excessive mutation since
///      the allocated storage can keep containing previous states. The return
///      value of the function is used to indicate whether the mutation was
///      successful, e.g., to limit the number of mutations or enable deferred
///      one-time assignment of the mutable component.
///
/// All storage classes must be registered with the uniquer via
/// `registerStorageType` using an appropriate unique `TypeID` for the storage
/// class.
class StorageUniquer {
public:
  StorageUniquer();
  ~StorageUniquer();

  /// Set the flag specifying if multi-threading is disabled within the uniquer.
  void disableMultithreading(bool disable = true);

  /// Register a new storage object with this uniquer using the given unique
  /// type id.
  void registerStorageType(TypeID id);

  /// This class acts as the base storage that all storage classes must derived
  /// from.
  class BaseStorage {
  public:
    /// Get the kind classification of this storage.
    unsigned getKind() const { return kind; }

  protected:
    BaseStorage() : kind(0) {}

  private:
    /// Allow access to the kind field.
    friend detail::StorageUniquerImpl;

    /// Classification of the subclass, used for type checking.
    unsigned kind;
  };

  /// This is a utility allocator used to allocate memory for instances of
  /// derived types.
  class StorageAllocator {
  public:
    /// Copy the specified array of elements into memory managed by our bump
    /// pointer allocator.  This assumes the elements are all PODs.
    template <typename T> ArrayRef<T> copyInto(ArrayRef<T> elements) {
      if (elements.empty())
        return llvm::None;
      auto result = allocator.Allocate<T>(elements.size());
      std::uninitialized_copy(elements.begin(), elements.end(), result);
      return ArrayRef<T>(result, elements.size());
    }

    /// Copy the provided string into memory managed by our bump pointer
    /// allocator.
    StringRef copyInto(StringRef str) {
      auto result = copyInto(ArrayRef<char>(str.data(), str.size()));
      return StringRef(result.data(), str.size());
    }

    /// Allocate an instance of the provided type.
    template <typename T> T *allocate() { return allocator.Allocate<T>(); }

    /// Allocate 'size' bytes of 'alignment' aligned memory.
    void *allocate(size_t size, size_t alignment) {
      return allocator.Allocate(size, alignment);
    }

  private:
    /// The raw allocator for type storage objects.
    llvm::BumpPtrAllocator allocator;
  };

  /// Gets a uniqued instance of 'Storage'. 'initFn' is an optional parameter
  /// that can be used to initialize a newly inserted storage instance. This
  /// function is used for derived types that have complex storage or uniquing
  /// constraints.
  template <typename Storage, typename Arg, typename... Args>
  Storage *get(const TypeID &id, function_ref<void(Storage *)> initFn,
               unsigned kind, Arg &&arg, Args &&...args) {
    // Construct a value of the derived key type.
    auto derivedKey =
        getKey<Storage>(std::forward<Arg>(arg), std::forward<Args>(args)...);

    // Create a hash of the kind and the derived key.
    unsigned hashValue = getHash<Storage>(kind, derivedKey);

    // Generate an equality function for the derived storage.
    auto isEqual = [&derivedKey](const BaseStorage *existing) {
      return static_cast<const Storage &>(*existing) == derivedKey;
    };

    // Generate a constructor function for the derived storage.
    auto ctorFn = [&](StorageAllocator &allocator) {
      auto *storage = Storage::construct(allocator, derivedKey);
      if (initFn)
        initFn(storage);
      return storage;
    };

    // Get an instance for the derived storage.
    return static_cast<Storage *>(
        getImpl(id, kind, hashValue, isEqual, ctorFn));
  }

  /// Gets a uniqued instance of 'Storage'. 'initFn' is an optional parameter
  /// that can be used to initialize a newly inserted storage instance. This
  /// function is used for derived types that use no additional storage or
  /// uniquing outside of the kind.
  template <typename Storage>
  Storage *get(const TypeID &id, function_ref<void(Storage *)> initFn,
               unsigned kind) {
    auto ctorFn = [&](StorageAllocator &allocator) {
      auto *storage = new (allocator.allocate<Storage>()) Storage();
      if (initFn)
        initFn(storage);
      return storage;
    };
    return static_cast<Storage *>(getImpl(id, kind, ctorFn));
  }

  /// Changes the mutable component of 'storage' by forwarding the trailing
  /// arguments to the 'mutate' function of the derived class.
  template <typename Storage, typename... Args>
  LogicalResult mutate(const TypeID &id, Storage *storage, Args &&...args) {
    auto mutationFn = [&](StorageAllocator &allocator) -> LogicalResult {
      return static_cast<Storage &>(*storage).mutate(
          allocator, std::forward<Args>(args)...);
    };
    return mutateImpl(id, mutationFn);
  }

  /// Erases a uniqued instance of 'Storage'. This function is used for derived
  /// types that have complex storage or uniquing constraints.
  template <typename Storage, typename Arg, typename... Args>
  void erase(const TypeID &id, unsigned kind, Arg &&arg, Args &&...args) {
    // Construct a value of the derived key type.
    auto derivedKey =
        getKey<Storage>(std::forward<Arg>(arg), std::forward<Args>(args)...);

    // Create a hash of the kind and the derived key.
    unsigned hashValue = getHash<Storage>(kind, derivedKey);

    // Generate an equality function for the derived storage.
    auto isEqual = [&derivedKey](const BaseStorage *existing) {
      return static_cast<const Storage &>(*existing) == derivedKey;
    };

    // Attempt to erase the storage instance.
    eraseImpl(id, kind, hashValue, isEqual, [](BaseStorage *storage) {
      static_cast<Storage *>(storage)->cleanup();
    });
  }

private:
  /// Implementation for getting/creating an instance of a derived type with
  /// complex storage.
  BaseStorage *getImpl(const TypeID &id, unsigned kind, unsigned hashValue,
                       function_ref<bool(const BaseStorage *)> isEqual,
                       function_ref<BaseStorage *(StorageAllocator &)> ctorFn);

  /// Implementation for getting/creating an instance of a derived type with
  /// default storage.
  BaseStorage *getImpl(const TypeID &id, unsigned kind,
                       function_ref<BaseStorage *(StorageAllocator &)> ctorFn);

  /// Implementation for erasing an instance of a derived type with complex
  /// storage.
  void eraseImpl(const TypeID &id, unsigned kind, unsigned hashValue,
                 function_ref<bool(const BaseStorage *)> isEqual,
                 function_ref<void(BaseStorage *)> cleanupFn);

  /// Implementation for mutating an instance of a derived storage.
  LogicalResult
  mutateImpl(const TypeID &id,
             function_ref<LogicalResult(StorageAllocator &)> mutationFn);

  /// The internal implementation class.
  std::unique_ptr<detail::StorageUniquerImpl> impl;

  //===--------------------------------------------------------------------===//
  // Key Construction
  //===--------------------------------------------------------------------===//

  /// Used to construct an instance of 'ImplTy::KeyTy' if there is an
  /// 'ImplTy::getKey' function for the provided arguments.
  template <typename ImplTy, typename... Args>
  static typename std::enable_if<
      llvm::is_detected<detail::has_impltype_getkey_t, ImplTy, Args...>::value,
      typename ImplTy::KeyTy>::type
  getKey(Args &&...args) {
    return ImplTy::getKey(args...);
  }
  /// If there is no 'ImplTy::getKey' method, then we try to directly construct
  /// the 'ImplTy::KeyTy' with the provided arguments.
  template <typename ImplTy, typename... Args>
  static typename std::enable_if<
      !llvm::is_detected<detail::has_impltype_getkey_t, ImplTy, Args...>::value,
      typename ImplTy::KeyTy>::type
  getKey(Args &&...args) {
    return typename ImplTy::KeyTy(args...);
  }

  //===--------------------------------------------------------------------===//
  // Key and Kind Hashing
  //===--------------------------------------------------------------------===//

  /// Used to generate a hash for the 'ImplTy::KeyTy' and kind of a storage
  /// instance if there is an 'ImplTy::hashKey' overload for 'DerivedKey'.
  template <typename ImplTy, typename DerivedKey>
  static typename std::enable_if<
      llvm::is_detected<detail::has_impltype_hash_t, ImplTy, DerivedKey>::value,
      ::llvm::hash_code>::type
  getHash(unsigned kind, const DerivedKey &derivedKey) {
    return llvm::hash_combine(kind, ImplTy::hashKey(derivedKey));
  }
  /// If there is no 'ImplTy::hashKey' default to using the
  /// 'llvm::DenseMapInfo' definition for 'DerivedKey' for generating a hash.
  template <typename ImplTy, typename DerivedKey>
  static typename std::enable_if<!llvm::is_detected<detail::has_impltype_hash_t,
                                                    ImplTy, DerivedKey>::value,
                                 ::llvm::hash_code>::type
  getHash(unsigned kind, const DerivedKey &derivedKey) {
    return llvm::hash_combine(
        kind, DenseMapInfo<DerivedKey>::getHashValue(derivedKey));
  }
};
} // end namespace mlir

#endif
