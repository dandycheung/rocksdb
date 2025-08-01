// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
#pragma once

#include <algorithm>
#include <limits>

#ifdef ROCKSDB_MALLOC_USABLE_SIZE
#ifdef OS_FREEBSD
#include <malloc_np.h>
#else  // OS_FREEBSD
#include <malloc.h>
#endif  // OS_FREEBSD
#endif  // ROCKSDB_MALLOC_USABLE_SIZE
#include <string>

#include "memory/memory_allocator_impl.h"
#include "port/likely.h"
#include "rocksdb/advanced_compression.h"
#include "rocksdb/options.h"
#include "table/block_based/block_type.h"
#include "test_util/sync_point.h"
#include "util/atomic.h"
#include "util/cast_util.h"
#include "util/coding.h"
#include "util/compression_context_cache.h"
#include "util/string_util.h"

#ifdef SNAPPY
#include <snappy-sinksource.h>
#include <snappy.h>
#endif

#ifdef ZLIB
#include <zlib.h>
#endif

#ifdef BZIP2
#include <bzlib.h>
#endif

#if defined(LZ4)
#include <lz4.h>
#include <lz4hc.h>
#if LZ4_VERSION_NUMBER < 10700  // < r129
#error "LZ4 support requires version >= 1.7.0 (lz4-devel)"
#endif
#endif

#ifdef ZSTD
#include <zstd.h>
#include <zstd_errors.h>
// ZSTD_Compress2(), ZSTD_compressStream2() and frame parameters all belong to
// advanced APIs and require v1.4.0+, which is from April 2019.
// https://github.com/facebook/zstd/blob/eb9f881eb810f2242f1ef36b3f3e7014eecb8fa6/lib/zstd.h#L297C40-L297C45
// To avoid a rat's nest of #ifdefs, we now require v1.4.0+ for ZSTD support.
#if ZSTD_VERSION_NUMBER < 10400
#error "ZSTD support requires version >= 1.4.0 (libzstd-devel)"
#endif  // ZSTD_VERSION_NUMBER
// The above release also includes digested dictionary support, but some
// required functions (ZSTD_createDDict_byReference) are still only exported
// with ZSTD_STATIC_LINKING_ONLY defined.
#if defined(ZSTD_STATIC_LINKING_ONLY)
#define ROCKSDB_ZSTD_DDICT
#endif  // defined(ZSTD_STATIC_LINKING_ONLY)
//  For ZDICT_* functions
#include <zdict.h>
// ZDICT_finalizeDictionary API is exported and stable since v1.4.5
#if ZSTD_VERSION_NUMBER >= 10405
#define ROCKSDB_ZDICT_FINALIZE
#endif  // ZSTD_VERSION_NUMBER >= 10405
#endif  // ZSTD

namespace ROCKSDB_NAMESPACE {
// Need this for the context allocation override
// On windows we need to do this explicitly
#if defined(ZSTD) && defined(ROCKSDB_JEMALLOC) && defined(OS_WIN) && \
    defined(ZSTD_STATIC_LINKING_ONLY)
#define ROCKSDB_ZSTD_CUSTOM_MEM
namespace port {
ZSTD_customMem GetJeZstdAllocationOverrides();
}  // namespace port
#endif  // defined(ZSTD) && defined(ROCKSDB_JEMALLOC) && defined(OS_WIN) &&
        // defined(ZSTD_STATIC_LINKING_ONLY)

// Cached data represents a portion that can be re-used
// If, in the future we have more than one native context to
// cache we can arrange this as a tuple
class ZSTDUncompressCachedData {
 public:
#if defined(ZSTD)
  using ZSTDNativeContext = ZSTD_DCtx*;
#else
  using ZSTDNativeContext = void*;
#endif  // ZSTD
  ZSTDUncompressCachedData() {}
  // Init from cache
  ZSTDUncompressCachedData(const ZSTDUncompressCachedData& o) = delete;
  ZSTDUncompressCachedData& operator=(const ZSTDUncompressCachedData&) = delete;
  ZSTDUncompressCachedData(ZSTDUncompressCachedData&& o) noexcept
      : ZSTDUncompressCachedData() {
    *this = std::move(o);
  }
  ZSTDUncompressCachedData& operator=(ZSTDUncompressCachedData&& o) noexcept {
    assert(zstd_ctx_ == nullptr);
    std::swap(zstd_ctx_, o.zstd_ctx_);
    std::swap(cache_idx_, o.cache_idx_);
    return *this;
  }
  ZSTDNativeContext Get() const { return zstd_ctx_; }
  int64_t GetCacheIndex() const { return cache_idx_; }
  void CreateIfNeeded() {
    if (zstd_ctx_ == nullptr) {
#if !defined(ZSTD)
      zstd_ctx_ = nullptr;
#elif defined(ROCKSDB_ZSTD_CUSTOM_MEM)
      zstd_ctx_ =
          ZSTD_createDCtx_advanced(port::GetJeZstdAllocationOverrides());
#else  // ZSTD && !ROCKSDB_ZSTD_CUSTOM_MEM
      zstd_ctx_ = ZSTD_createDCtx();
#endif
      cache_idx_ = -1;
    }
  }
  void InitFromCache(const ZSTDUncompressCachedData& o, int64_t idx) {
    zstd_ctx_ = o.zstd_ctx_;
    cache_idx_ = idx;
  }
  ~ZSTDUncompressCachedData() {
#if defined(ZSTD)
    if (zstd_ctx_ != nullptr && cache_idx_ == -1) {
      ZSTD_freeDCtx(zstd_ctx_);
    }
#endif  // ZSTD
  }

 private:
  ZSTDNativeContext zstd_ctx_ = nullptr;
  int64_t cache_idx_ = -1;  // -1 means this instance owns the context
};
}  // namespace ROCKSDB_NAMESPACE

#if defined(XPRESS)
#include "port/xpress.h"
#endif

namespace ROCKSDB_NAMESPACE {

class FailureDecompressor : public Decompressor {
 public:
  explicit FailureDecompressor(Status&& status) : status_(std::move(status)) {
    assert(!status_.ok());
  }
  ~FailureDecompressor() override { status_.PermitUncheckedError(); }

  const char* Name() const override { return "FailureDecompressor"; }

  Status ExtractUncompressedSize(Args& /*args*/) override { return status_; }

  Status DecompressBlock(const Args& /*args*/,
                         char* /*uncompressed_output*/) override {
    return status_;
  }

 protected:
  Status status_;
};

// Owns a decompression dictionary, and associated Decompressor, for storing
// in the block cache.
//
// Justification: for a "processed" dictionary to be saved in block cache, we
// also need a reference to the decompressor that processed it, to ensure it
// is recognized properly. At that point, we might as well have the dictionary
// part of the decompressor identity and track an associated decompressor along
// with a decompression dictionary in the block cache, and the decompressor
// hides potential details of processing the dictionary.
struct DecompressorDict {
  // Block containing the data for the compression dictionary in case the
  // constructor that takes a string parameter is used.
  std::string dict_str_;

  // Block containing the data for the compression dictionary in case the
  // constructor that takes a Slice parameter is used and the passed in
  // CacheAllocationPtr is not nullptr.
  CacheAllocationPtr dict_allocation_;

  // A Decompressor referencing and using the dictionary owned by this.
  std::unique_ptr<Decompressor> decompressor_;

  // Approximate owned memory usage
  size_t memory_usage_;

  DecompressorDict(std::string&& dict, Decompressor& from_decompressor)
      : dict_str_(std::move(dict)) {
    Populate(from_decompressor, dict_str_);
  }

  DecompressorDict(Slice slice, CacheAllocationPtr&& allocation,
                   Decompressor& from_decompressor)
      : dict_allocation_(std::move(allocation)) {
    Populate(from_decompressor, slice);
  }

  DecompressorDict(DecompressorDict&& rhs) noexcept
      : dict_str_(std::move(rhs.dict_str_)),
        dict_allocation_(std::move(rhs.dict_allocation_)),
        decompressor_(std::move(rhs.decompressor_)),
        memory_usage_(std::move(rhs.memory_usage_)) {}

  DecompressorDict& operator=(DecompressorDict&& rhs) noexcept {
    if (this == &rhs) {
      return *this;
    }
    dict_str_ = std::move(rhs.dict_str_);
    dict_allocation_ = std::move(rhs.dict_allocation_);
    decompressor_ = std::move(rhs.decompressor_);
    return *this;
  }
  // Disable copy
  DecompressorDict(const DecompressorDict&) = delete;
  DecompressorDict& operator=(const DecompressorDict&) = delete;

  // The object is self-contained if the string constructor is used, or the
  // Slice constructor is invoked with a non-null allocation. Otherwise, it
  // is the caller's responsibility to ensure that the underlying storage
  // outlives this object.
  bool own_bytes() const { return !dict_str_.empty() || dict_allocation_; }

  const Slice& GetRawDict() const { return decompressor_->GetSerializedDict(); }

  // For TypedCacheInterface
  const Slice& ContentSlice() const { return GetRawDict(); }
  static constexpr CacheEntryRole kCacheEntryRole = CacheEntryRole::kOtherBlock;
  static constexpr BlockType kBlockType = BlockType::kCompressionDictionary;

  size_t ApproximateMemoryUsage() const { return memory_usage_; }

 private:
  void Populate(Decompressor& from_decompressor, Slice dict) {
    if (UNLIKELY(dict.empty())) {
      dict_str_ = {};
      dict_allocation_ = {};
      // Appropriately reject bad files with empty dictionary block.
      // It is longstanding not to write an empty dictionary block:
      // https://github.com/facebook/rocksdb/blame/10.2.fb/table/block_based/block_based_table_builder.cc#L1841
      decompressor_ = std::make_unique<FailureDecompressor>(
          Status::Corruption("Decompression dictionary is empty"));
    } else {
      Status s = from_decompressor.MaybeCloneForDict(dict, &decompressor_);
      if (decompressor_ == nullptr) {
        dict_str_ = {};
        dict_allocation_ = {};
        assert(!s.ok());
        decompressor_ = std::make_unique<FailureDecompressor>(std::move(s));
      } else {
        assert(s.ok());
        assert(decompressor_->GetSerializedDict() == dict);
      }
    }

    memory_usage_ = sizeof(struct DecompressorDict);
    memory_usage_ += dict_str_.size();
    if (dict_allocation_) {
      auto allocator = dict_allocation_.get_deleter().allocator;
      if (allocator) {
        memory_usage_ +=
            allocator->UsableSize(dict_allocation_.get(), GetRawDict().size());
      } else {
        memory_usage_ += GetRawDict().size();
      }
    }
    memory_usage_ += decompressor_->ApproximateOwnedMemoryUsage();
  }
};

// Holds dictionary and related data, like ZSTD's digested compression
// dictionary.
struct CompressionDict {
#ifdef ZSTD
  ZSTD_CDict* zstd_cdict_ = nullptr;
#endif  // ZSTD
  std::string dict_;

 public:
  CompressionDict() = default;
  CompressionDict(std::string&& dict, CompressionType type, int level) {
    dict_ = std::move(dict);
#ifdef ZSTD
    zstd_cdict_ = nullptr;
    if (!dict_.empty() && type == kZSTD) {
      if (level == CompressionOptions::kDefaultCompressionLevel) {
        // NB: ZSTD_CLEVEL_DEFAULT is historically == 3
        level = ZSTD_CLEVEL_DEFAULT;
      }
      // Should be safe (but slower) if below call fails as we'll use the
      // raw dictionary to compress.
      zstd_cdict_ = ZSTD_createCDict(dict_.data(), dict_.size(), level);
      assert(zstd_cdict_ != nullptr);
    }
#else
    (void)type;
    (void)level;
#endif  // ZSTD
  }

  CompressionDict(CompressionDict&& other) {
#ifdef ZSTD
    zstd_cdict_ = other.zstd_cdict_;
    other.zstd_cdict_ = nullptr;
#endif  // ZSTD
    dict_ = std::move(other.dict_);
  }
  CompressionDict& operator=(CompressionDict&& other) {
    if (this == &other) {
      return *this;
    }
#ifdef ZSTD
    zstd_cdict_ = other.zstd_cdict_;
    other.zstd_cdict_ = nullptr;
#endif  // ZSTD
    dict_ = std::move(other.dict_);
    return *this;
  }

  ~CompressionDict() {
#ifdef ZSTD
    size_t res = 0;
    if (zstd_cdict_ != nullptr) {
      res = ZSTD_freeCDict(zstd_cdict_);
    }
    assert(res == 0);  // Last I checked they can't fail
    (void)res;         // prevent unused var warning
#endif                 // ZSTD
  }

#ifdef ZSTD
  const ZSTD_CDict* GetDigestedZstdCDict() const { return zstd_cdict_; }
#endif  // ZSTD

  Slice GetRawDict() const { return dict_; }
  bool empty() const { return dict_.empty(); }

  static const CompressionDict& GetEmptyDict() {
    static CompressionDict empty_dict{};
    return empty_dict;
  }

  // Disable copy
  CompressionDict(const CompressionDict&) = delete;
  CompressionDict& operator=(const CompressionDict&) = delete;
};

// Holds dictionary and related data, like ZSTD's digested uncompression
// dictionary.
struct UncompressionDict {
  // Block containing the data for the compression dictionary in case the
  // constructor that takes a string parameter is used.
  std::string dict_;

  // Block containing the data for the compression dictionary in case the
  // constructor that takes a Slice parameter is used and the passed in
  // CacheAllocationPtr is not nullptr.
  CacheAllocationPtr allocation_;

  // Slice pointing to the compression dictionary data. Can point to
  // dict_, allocation_, or some other memory location, depending on how
  // the object was constructed.
  Slice slice_;

#ifdef ROCKSDB_ZSTD_DDICT
  // Processed version of the contents of slice_ for ZSTD compression.
  ZSTD_DDict* zstd_ddict_ = nullptr;
#endif  // ROCKSDB_ZSTD_DDICT

  UncompressionDict(std::string&& dict, bool using_zstd)
      : dict_(std::move(dict)), slice_(dict_) {
#ifdef ROCKSDB_ZSTD_DDICT
    if (!slice_.empty() && using_zstd) {
      zstd_ddict_ = ZSTD_createDDict_byReference(slice_.data(), slice_.size());
      assert(zstd_ddict_ != nullptr);
    }
#else
    (void)using_zstd;
#endif  // ROCKSDB_ZSTD_DDICT
  }

  UncompressionDict(Slice slice, CacheAllocationPtr&& allocation,
                    bool using_zstd)
      : allocation_(std::move(allocation)), slice_(std::move(slice)) {
#ifdef ROCKSDB_ZSTD_DDICT
    if (!slice_.empty() && using_zstd) {
      zstd_ddict_ = ZSTD_createDDict_byReference(slice_.data(), slice_.size());
      assert(zstd_ddict_ != nullptr);
    }
#else
    (void)using_zstd;
#endif  // ROCKSDB_ZSTD_DDICT
  }

  UncompressionDict(UncompressionDict&& rhs)
      : dict_(std::move(rhs.dict_)),
        allocation_(std::move(rhs.allocation_)),
        slice_(std::move(rhs.slice_))
#ifdef ROCKSDB_ZSTD_DDICT
        ,
        zstd_ddict_(rhs.zstd_ddict_)
#endif
  {
#ifdef ROCKSDB_ZSTD_DDICT
    rhs.zstd_ddict_ = nullptr;
#endif
  }

  ~UncompressionDict() {
#ifdef ROCKSDB_ZSTD_DDICT
    size_t res = 0;
    if (zstd_ddict_ != nullptr) {
      res = ZSTD_freeDDict(zstd_ddict_);
    }
    assert(res == 0);  // Last I checked they can't fail
    (void)res;         // prevent unused var warning
#endif                 // ROCKSDB_ZSTD_DDICT
  }

  UncompressionDict& operator=(UncompressionDict&& rhs) {
    if (this == &rhs) {
      return *this;
    }

    dict_ = std::move(rhs.dict_);
    allocation_ = std::move(rhs.allocation_);
    slice_ = std::move(rhs.slice_);

#ifdef ROCKSDB_ZSTD_DDICT
    zstd_ddict_ = rhs.zstd_ddict_;
    rhs.zstd_ddict_ = nullptr;
#endif

    return *this;
  }

  // The object is self-contained if the string constructor is used, or the
  // Slice constructor is invoked with a non-null allocation. Otherwise, it
  // is the caller's responsibility to ensure that the underlying storage
  // outlives this object.
  bool own_bytes() const { return !dict_.empty() || allocation_; }

  const Slice& GetRawDict() const { return slice_; }

  // For TypedCacheInterface
  const Slice& ContentSlice() const { return slice_; }
  static constexpr CacheEntryRole kCacheEntryRole = CacheEntryRole::kOtherBlock;
  static constexpr BlockType kBlockType = BlockType::kCompressionDictionary;

#ifdef ROCKSDB_ZSTD_DDICT
  const ZSTD_DDict* GetDigestedZstdDDict() const { return zstd_ddict_; }
#endif  // ROCKSDB_ZSTD_DDICT

  static const UncompressionDict& GetEmptyDict() {
    static UncompressionDict empty_dict{};
    return empty_dict;
  }

  size_t ApproximateMemoryUsage() const {
    size_t usage = sizeof(struct UncompressionDict);
    usage += dict_.size();
    if (allocation_) {
      auto allocator = allocation_.get_deleter().allocator;
      if (allocator) {
        usage += allocator->UsableSize(allocation_.get(), slice_.size());
      } else {
        usage += slice_.size();
      }
    }
#ifdef ROCKSDB_ZSTD_DDICT
    usage += ZSTD_sizeof_DDict(zstd_ddict_);
#endif  // ROCKSDB_ZSTD_DDICT
    return usage;
  }

  UncompressionDict() = default;
  // Disable copy
  UncompressionDict(const CompressionDict&) = delete;
  UncompressionDict& operator=(const CompressionDict&) = delete;
};

class CompressionContext : public Compressor::WorkingArea {
 private:
#ifdef ZSTD
  ZSTD_CCtx* zstd_ctx_ = nullptr;

  ZSTD_CCtx* CreateZSTDContext() {
#ifdef ROCKSDB_ZSTD_CUSTOM_MEM
    return ZSTD_createCCtx_advanced(port::GetJeZstdAllocationOverrides());
#else   // ROCKSDB_ZSTD_CUSTOM_MEM
    return ZSTD_createCCtx();
#endif  // ROCKSDB_ZSTD_CUSTOM_MEM
  }

 public:
  // callable inside ZSTD_Compress
  ZSTD_CCtx* ZSTDPreallocCtx() const {
    assert(zstd_ctx_ != nullptr);
    return zstd_ctx_;
  }

 private:
#endif  // ZSTD

  void CreateNativeContext(CompressionType type, int level, bool checksum) {
#ifdef ZSTD
    if (type == kZSTD) {
      zstd_ctx_ = CreateZSTDContext();
      if (level == CompressionOptions::kDefaultCompressionLevel) {
        // NB: ZSTD_CLEVEL_DEFAULT is historically == 3
        level = ZSTD_CLEVEL_DEFAULT;
      }
      size_t err =
          ZSTD_CCtx_setParameter(zstd_ctx_, ZSTD_c_compressionLevel, level);
      if (ZSTD_isError(err)) {
        assert(false);
        ZSTD_freeCCtx(zstd_ctx_);
        zstd_ctx_ = CreateZSTDContext();
      }
      if (checksum) {
        err = ZSTD_CCtx_setParameter(zstd_ctx_, ZSTD_c_checksumFlag, 1);
        if (ZSTD_isError(err)) {
          assert(false);
          ZSTD_freeCCtx(zstd_ctx_);
          zstd_ctx_ = CreateZSTDContext();
        }
      }
    }
#else
    (void)type;
    (void)level;
    (void)checksum;
#endif  // ZSTD
  }
  void DestroyNativeContext() {
#ifdef ZSTD
    if (zstd_ctx_ != nullptr) {
      ZSTD_freeCCtx(zstd_ctx_);
    }
#endif  // ZSTD
  }

 public:
  explicit CompressionContext(CompressionType type,
                              const CompressionOptions& options) {
    CreateNativeContext(type, options.level, options.checksum);
  }
  ~CompressionContext() { DestroyNativeContext(); }
  CompressionContext(const CompressionContext&) = delete;
  CompressionContext& operator=(const CompressionContext&) = delete;
};

// TODO: rename
class CompressionInfo {
  const CompressionOptions& opts_;
  const CompressionContext& context_;
  const CompressionDict& dict_;
  const CompressionType type_;

 public:
  CompressionInfo(const CompressionOptions& _opts,
                  const CompressionContext& _context,
                  const CompressionDict& _dict, CompressionType _type)
      : opts_(_opts), context_(_context), dict_(_dict), type_(_type) {}

  const CompressionOptions& options() const { return opts_; }
  const CompressionContext& context() const { return context_; }
  const CompressionDict& dict() const { return dict_; }
  CompressionType type() const { return type_; }
};

// This is like a working area, reusable for different dicts, etc.
// TODO: refactor / consolidate
class UncompressionContext : public Decompressor::WorkingArea {
 private:
  CompressionContextCache* ctx_cache_ = nullptr;
  ZSTDUncompressCachedData uncomp_cached_data_;

 public:
  explicit UncompressionContext(CompressionType type) {
    if (type == kZSTD) {
      ctx_cache_ = CompressionContextCache::Instance();
      uncomp_cached_data_ = ctx_cache_->GetCachedZSTDUncompressData();
    }
  }
  ~UncompressionContext() {
    if (uncomp_cached_data_.GetCacheIndex() != -1) {
      assert(ctx_cache_ != nullptr);
      ctx_cache_->ReturnCachedZSTDUncompressData(
          uncomp_cached_data_.GetCacheIndex());
    }
  }
  UncompressionContext(const UncompressionContext&) = delete;
  UncompressionContext& operator=(const UncompressionContext&) = delete;

  ZSTDUncompressCachedData::ZSTDNativeContext GetZSTDContext() const {
    return uncomp_cached_data_.Get();
  }
};

class UncompressionInfo {
  const UncompressionContext& context_;
  const UncompressionDict& dict_;
  const CompressionType type_;

 public:
  UncompressionInfo(const UncompressionContext& _context,
                    const UncompressionDict& _dict, CompressionType _type)
      : context_(_context), dict_(_dict), type_(_type) {}

  const UncompressionContext& context() const { return context_; }
  const UncompressionDict& dict() const { return dict_; }
  CompressionType type() const { return type_; }
};

inline bool Snappy_Supported() {
#ifdef SNAPPY
  return true;
#else
  return false;
#endif
}

inline bool Zlib_Supported() {
#ifdef ZLIB
  return true;
#else
  return false;
#endif
}

inline bool BZip2_Supported() {
#ifdef BZIP2
  return true;
#else
  return false;
#endif
}

inline bool LZ4_Supported() {
#ifdef LZ4
  return true;
#else
  return false;
#endif
}

inline bool XPRESS_Supported() {
#ifdef XPRESS
  return true;
#else
  return false;
#endif
}

inline bool ZSTD_Supported() {
#ifdef ZSTD
  // NB: ZSTD format is finalized since version 0.8.0. See ZSTD_VERSION_NUMBER
  // check above.
  return true;
#else
  return false;
#endif
}

inline bool ZSTD_Streaming_Supported() {
#if defined(ZSTD)
  return true;
#else
  return false;
#endif
}

inline bool StreamingCompressionTypeSupported(
    CompressionType compression_type) {
  switch (compression_type) {
    case kNoCompression:
      return true;
    case kZSTD:
      return ZSTD_Streaming_Supported();
    default:
      return false;
  }
}

inline bool CompressionTypeSupported(CompressionType compression_type) {
  switch (compression_type) {
    case kNoCompression:
      return true;
    case kSnappyCompression:
      return Snappy_Supported();
    case kZlibCompression:
      return Zlib_Supported();
    case kBZip2Compression:
      return BZip2_Supported();
    case kLZ4Compression:
      return LZ4_Supported();
    case kLZ4HCCompression:
      return LZ4_Supported();
    case kXpressCompression:
      return XPRESS_Supported();
    case kZSTD:
      return ZSTD_Supported();
    default:  // Including custom compression types
      return false;
  }
}

inline bool DictCompressionTypeSupported(CompressionType compression_type) {
  switch (compression_type) {
    case kNoCompression:
      return false;
    case kSnappyCompression:
      return false;
    case kZlibCompression:
      return Zlib_Supported();
    case kBZip2Compression:
      return false;
    case kLZ4Compression:
    case kLZ4HCCompression:
#if LZ4_VERSION_NUMBER >= 10400  // r124+
      return LZ4_Supported();
#else
      return false;
#endif
    case kXpressCompression:
      return false;
    case kZSTD:
      // NB: dictionary supported since 0.5.0. See ZSTD_VERSION_NUMBER check
      // above.
      return ZSTD_Supported();
    default:  // Including custom compression types
      return false;
  }
}

// WART: does not match OptionsHelper::compression_type_string_map
inline std::string CompressionTypeToString(CompressionType compression_type) {
  switch (compression_type) {
    case kNoCompression:
      return "NoCompression";
    case kSnappyCompression:
      return "Snappy";
    case kZlibCompression:
      return "Zlib";
    case kBZip2Compression:
      return "BZip2";
    case kLZ4Compression:
      return "LZ4";
    case kLZ4HCCompression:
      return "LZ4HC";
    case kXpressCompression:
      return "Xpress";
    case kZSTD:
      return "ZSTD";
    case kDisableCompressionOption:
      return "DisableOption";
    default: {
      bool is_custom = compression_type >= kFirstCustomCompression &&
                       compression_type <= kLastCustomCompression;
      unsigned char c = lossless_cast<unsigned char>(compression_type);
      return (is_custom ? "Custom" : "Reserved") +
             ToBaseCharsString<16>(2, c, /*uppercase=*/true);
    }
  }
}

// WART: does not match OptionsHelper::compression_type_string_map
inline CompressionType CompressionTypeFromString(
    std::string compression_type_str) {
  if (!compression_type_str.empty()) {
    switch (compression_type_str[0]) {
      case 'N':
        if (compression_type_str == "NoCompression") {
          return kNoCompression;
        }
        break;
      case 'S':
        if (compression_type_str == "Snappy") {
          return kSnappyCompression;
        }
        break;
      case 'Z':
        if (compression_type_str == "ZSTD") {
          return kZSTD;
        }
        if (compression_type_str == "Zlib") {
          return kZlibCompression;
        }
        break;
      case 'B':
        if (compression_type_str == "BZip2") {
          return kBZip2Compression;
        }
        break;
      case 'L':
        if (compression_type_str == "LZ4") {
          return kLZ4Compression;
        }
        if (compression_type_str == "LZ4HC") {
          return kLZ4HCCompression;
        }
        break;
      case 'X':
        if (compression_type_str == "Xpress") {
          return kXpressCompression;
        }
        break;
      default:;
    }
  }
  // unrecognized
  return kDisableCompressionOption;
}

inline std::string CompressionOptionsToString(
    const CompressionOptions& compression_options) {
  std::string result;
  result.reserve(512);
  result.append("window_bits=")
      .append(std::to_string(compression_options.window_bits))
      .append("; ");
  result.append("level=")
      .append(std::to_string(compression_options.level))
      .append("; ");
  result.append("strategy=")
      .append(std::to_string(compression_options.strategy))
      .append("; ");
  result.append("max_dict_bytes=")
      .append(std::to_string(compression_options.max_dict_bytes))
      .append("; ");
  result.append("zstd_max_train_bytes=")
      .append(std::to_string(compression_options.zstd_max_train_bytes))
      .append("; ");
  // NOTE: parallel_threads is skipped because it doesn't really affect the file
  // contents written, arguably doesn't belong in CompressionOptions
  result.append("enabled=")
      .append(std::to_string(compression_options.enabled))
      .append("; ");
  result.append("max_dict_buffer_bytes=")
      .append(std::to_string(compression_options.max_dict_buffer_bytes))
      .append("; ");
  result.append("use_zstd_dict_trainer=")
      .append(std::to_string(compression_options.use_zstd_dict_trainer))
      .append("; ");
  result.append("max_compressed_bytes_per_kb=")
      .append(std::to_string(compression_options.max_compressed_bytes_per_kb))
      .append("; ");
  result.append("checksum=")
      .append(std::to_string(compression_options.checksum))
      .append("; ");
  return result;
}

// compress_format_version can have two values:
// 1 -- decompressed sizes for BZip2 and Zlib are not included in the compressed
// block. Also, decompressed sizes for LZ4 are encoded in platform-dependent
// way.
// 2 -- Zlib, BZip2 and LZ4 encode decompressed size as Varint32 just before the
// start of compressed block. Snappy and XPRESS instead extract the decompressed
// size from the compressed block itself, same as version 1.

inline bool Snappy_Compress(const CompressionInfo& /*info*/, const char* input,
                            size_t length, ::std::string* output) {
#ifdef SNAPPY
  output->resize(snappy::MaxCompressedLength(length));
  size_t outlen;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
  return true;
#else
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

inline CacheAllocationPtr Snappy_Uncompress(
    const char* input, size_t length, size_t* uncompressed_size,
    MemoryAllocator* allocator = nullptr) {
#ifdef SNAPPY
  size_t uncompressed_length = 0;
  if (!snappy::GetUncompressedLength(input, length, &uncompressed_length)) {
    return nullptr;
  }

  CacheAllocationPtr output = AllocateBlock(uncompressed_length, allocator);

  if (!snappy::RawUncompress(input, length, output.get())) {
    return nullptr;
  }

  *uncompressed_size = uncompressed_length;

  return output;
#else
  (void)input;
  (void)length;
  (void)uncompressed_size;
  (void)allocator;
  return nullptr;
#endif
}

namespace compression {
// returns size
inline size_t PutDecompressedSizeInfo(std::string* output, uint32_t length) {
  PutVarint32(output, length);
  return output->size();
}

inline bool GetDecompressedSizeInfo(const char** input_data,
                                    size_t* input_length,
                                    uint32_t* output_len) {
  auto new_input_data =
      GetVarint32Ptr(*input_data, *input_data + *input_length, output_len);
  if (new_input_data == nullptr) {
    return false;
  }
  *input_length -= (new_input_data - *input_data);
  *input_data = new_input_data;
  return true;
}
}  // namespace compression

// compress_format_version == 1 -- decompressed size is not included in the
// block header
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
// @param compression_dict Data for presetting the compression library's
//    dictionary.
inline bool Zlib_Compress(const CompressionInfo& info,
                          uint32_t compress_format_version, const char* input,
                          size_t length, ::std::string* output) {
#ifdef ZLIB
  if (length > std::numeric_limits<uint32_t>::max()) {
    // Can't compress more than 4GB
    return false;
  }

  size_t output_header_len = 0;
  if (compress_format_version == 2) {
    output_header_len = compression::PutDecompressedSizeInfo(
        output, static_cast<uint32_t>(length));
  }

  // The memLevel parameter specifies how much memory should be allocated for
  // the internal compression state.
  // memLevel=1 uses minimum memory but is slow and reduces compression ratio.
  // memLevel=9 uses maximum memory for optimal speed.
  // The default value is 8. See zconf.h for more details.
  static const int memLevel = 8;
  int level;
  if (info.options().level == CompressionOptions::kDefaultCompressionLevel) {
    level = Z_DEFAULT_COMPRESSION;
  } else {
    level = info.options().level;
  }
  z_stream _stream;
  memset(&_stream, 0, sizeof(z_stream));
  int st = deflateInit2(&_stream, level, Z_DEFLATED, info.options().window_bits,
                        memLevel, info.options().strategy);
  if (st != Z_OK) {
    return false;
  }

  Slice compression_dict = info.dict().GetRawDict();
  if (compression_dict.size()) {
    // Initialize the compression library's dictionary
    st = deflateSetDictionary(
        &_stream, reinterpret_cast<const Bytef*>(compression_dict.data()),
        static_cast<unsigned int>(compression_dict.size()));
    if (st != Z_OK) {
      deflateEnd(&_stream);
      return false;
    }
  }

  // Get an upper bound on the compressed size.
  size_t upper_bound =
      deflateBound(&_stream, static_cast<unsigned long>(length));
  output->resize(output_header_len + upper_bound);

  // Compress the input, and put compressed data in output.
  _stream.next_in = (Bytef*)input;
  _stream.avail_in = static_cast<unsigned int>(length);

  // Initialize the output size.
  _stream.avail_out = static_cast<unsigned int>(upper_bound);
  _stream.next_out = reinterpret_cast<Bytef*>(&(*output)[output_header_len]);

  bool compressed = false;
  st = deflate(&_stream, Z_FINISH);
  if (st == Z_STREAM_END) {
    compressed = true;
    output->resize(output->size() - _stream.avail_out);
  }
  // The only return value we really care about is Z_STREAM_END.
  // Z_OK means insufficient output space. This means the compression is
  // bigger than decompressed size. Just fail the compression in that case.

  deflateEnd(&_stream);
  return compressed;
#else
  (void)info;
  (void)compress_format_version;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

// compress_format_version == 1 -- decompressed size is not included in the
// block header
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
// @param compression_dict Data for presetting the compression library's
//    dictionary.
inline CacheAllocationPtr Zlib_Uncompress(
    const UncompressionInfo& info, const char* input_data, size_t input_length,
    size_t* uncompressed_size, uint32_t compress_format_version,
    MemoryAllocator* allocator = nullptr, int windowBits = -14) {
#ifdef ZLIB
  uint32_t output_len = 0;
  if (compress_format_version == 2) {
    if (!compression::GetDecompressedSizeInfo(&input_data, &input_length,
                                              &output_len)) {
      return nullptr;
    }
  } else {
    // Assume the decompressed data size will 5x of compressed size, but round
    // to the page size
    size_t proposed_output_len = ((input_length * 5) & (~(4096 - 1))) + 4096;
    output_len = static_cast<uint32_t>(
        std::min(proposed_output_len,
                 static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
  }

  z_stream _stream;
  memset(&_stream, 0, sizeof(z_stream));

  // For raw inflate, the windowBits should be -8..-15.
  // If windowBits is bigger than zero, it will use either zlib
  // header or gzip header. Adding 32 to it will do automatic detection.
  int st =
      inflateInit2(&_stream, windowBits > 0 ? windowBits + 32 : windowBits);
  if (st != Z_OK) {
    return nullptr;
  }

  const Slice& compression_dict = info.dict().GetRawDict();
  if (compression_dict.size()) {
    // Initialize the compression library's dictionary
    st = inflateSetDictionary(
        &_stream, reinterpret_cast<const Bytef*>(compression_dict.data()),
        static_cast<unsigned int>(compression_dict.size()));
    if (st != Z_OK) {
      return nullptr;
    }
  }

  _stream.next_in = (Bytef*)input_data;
  _stream.avail_in = static_cast<unsigned int>(input_length);

  auto output = AllocateBlock(output_len, allocator);

  _stream.next_out = (Bytef*)output.get();
  _stream.avail_out = static_cast<unsigned int>(output_len);

  bool done = false;
  while (!done) {
    st = inflate(&_stream, Z_SYNC_FLUSH);
    switch (st) {
      case Z_STREAM_END:
        done = true;
        break;
      case Z_OK: {
        // No output space. Increase the output space by 20%.
        // We should never run out of output space if
        // compress_format_version == 2
        assert(compress_format_version != 2);
        size_t old_sz = output_len;
        uint32_t output_len_delta = output_len / 5;
        output_len += output_len_delta < 10 ? 10 : output_len_delta;
        auto tmp = AllocateBlock(output_len, allocator);
        memcpy(tmp.get(), output.get(), old_sz);
        output = std::move(tmp);

        // Set more output.
        _stream.next_out = (Bytef*)(output.get() + old_sz);
        _stream.avail_out = static_cast<unsigned int>(output_len - old_sz);
        break;
      }
      case Z_BUF_ERROR:
      default:
        inflateEnd(&_stream);
        return nullptr;
    }
  }

  // If we encoded decompressed block size, we should have no bytes left
  assert(compress_format_version != 2 || _stream.avail_out == 0);
  assert(output_len >= _stream.avail_out);
  *uncompressed_size = output_len - _stream.avail_out;
  inflateEnd(&_stream);
  return output;
#else
  (void)info;
  (void)input_data;
  (void)input_length;
  (void)uncompressed_size;
  (void)compress_format_version;
  (void)allocator;
  (void)windowBits;
  return nullptr;
#endif
}

// compress_format_version == 1 -- decompressed size is not included in the
// block header
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
inline bool BZip2_Compress(const CompressionInfo& /*info*/,
                           uint32_t compress_format_version, const char* input,
                           size_t length, ::std::string* output) {
#ifdef BZIP2
  if (length > std::numeric_limits<uint32_t>::max()) {
    // Can't compress more than 4GB
    return false;
  }
  size_t output_header_len = 0;
  if (compress_format_version == 2) {
    output_header_len = compression::PutDecompressedSizeInfo(
        output, static_cast<uint32_t>(length));
  }
  // Resize output to be the plain data length.
  // This may not be big enough if the compression actually expands data.
  output->resize(output_header_len + length);

  bz_stream _stream;
  memset(&_stream, 0, sizeof(bz_stream));

  // Block size 1 is 100K.
  // 0 is for silent.
  // 30 is the default workFactor
  int st = BZ2_bzCompressInit(&_stream, 1, 0, 30);
  if (st != BZ_OK) {
    return false;
  }

  // Compress the input, and put compressed data in output.
  _stream.next_in = (char*)input;
  _stream.avail_in = static_cast<unsigned int>(length);

  // Initialize the output size.
  _stream.avail_out = static_cast<unsigned int>(length);
  _stream.next_out = output->data() + output_header_len;

  bool compressed = false;
  st = BZ2_bzCompress(&_stream, BZ_FINISH);
  if (st == BZ_STREAM_END) {
    compressed = true;
    output->resize(output->size() - _stream.avail_out);
  }
  // The only return value we really care about is BZ_STREAM_END.
  // BZ_FINISH_OK means insufficient output space. This means the compression
  // is bigger than decompressed size. Just fail the compression in that case.

  BZ2_bzCompressEnd(&_stream);
  return compressed;
#else
  (void)compress_format_version;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

// compress_format_version == 1 -- decompressed size is not included in the
// block header
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
inline CacheAllocationPtr BZip2_Uncompress(
    const char* input_data, size_t input_length, size_t* uncompressed_size,
    uint32_t compress_format_version, MemoryAllocator* allocator = nullptr) {
#ifdef BZIP2
  uint32_t output_len = 0;
  if (compress_format_version == 2) {
    if (!compression::GetDecompressedSizeInfo(&input_data, &input_length,
                                              &output_len)) {
      return nullptr;
    }
  } else {
    // Assume the decompressed data size will 5x of compressed size, but round
    // to the next page size
    size_t proposed_output_len = ((input_length * 5) & (~(4096 - 1))) + 4096;
    output_len = static_cast<uint32_t>(
        std::min(proposed_output_len,
                 static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
  }

  bz_stream _stream;
  memset(&_stream, 0, sizeof(bz_stream));

  int st = BZ2_bzDecompressInit(&_stream, 0, 0);
  if (st != BZ_OK) {
    return nullptr;
  }

  _stream.next_in = (char*)input_data;
  _stream.avail_in = static_cast<unsigned int>(input_length);

  auto output = AllocateBlock(output_len, allocator);

  _stream.next_out = (char*)output.get();
  _stream.avail_out = static_cast<unsigned int>(output_len);

  bool done = false;
  while (!done) {
    st = BZ2_bzDecompress(&_stream);
    switch (st) {
      case BZ_STREAM_END:
        done = true;
        break;
      case BZ_OK: {
        // No output space. Increase the output space by 20%.
        // We should never run out of output space if
        // compress_format_version == 2
        assert(compress_format_version != 2);
        uint32_t old_sz = output_len;
        output_len = output_len * 1.2;
        auto tmp = AllocateBlock(output_len, allocator);
        memcpy(tmp.get(), output.get(), old_sz);
        output = std::move(tmp);

        // Set more output.
        _stream.next_out = (char*)(output.get() + old_sz);
        _stream.avail_out = static_cast<unsigned int>(output_len - old_sz);
        break;
      }
      default:
        BZ2_bzDecompressEnd(&_stream);
        return nullptr;
    }
  }

  // If we encoded decompressed block size, we should have no bytes left
  assert(compress_format_version != 2 || _stream.avail_out == 0);
  assert(output_len >= _stream.avail_out);
  *uncompressed_size = output_len - _stream.avail_out;
  BZ2_bzDecompressEnd(&_stream);
  return output;
#else
  (void)input_data;
  (void)input_length;
  (void)uncompressed_size;
  (void)compress_format_version;
  (void)allocator;
  return nullptr;
#endif
}

// compress_format_version == 1 -- decompressed size is included in the
// block header using memcpy, which makes database non-portable)
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
// @param compression_dict Data for presetting the compression library's
//    dictionary.
inline bool LZ4_Compress(const CompressionInfo& info,
                         uint32_t compress_format_version, const char* input,
                         size_t length, ::std::string* output) {
#ifdef LZ4
  if (length > std::numeric_limits<uint32_t>::max()) {
    // Can't compress more than 4GB
    return false;
  }

  size_t output_header_len = 0;
  if (compress_format_version == 2) {
    // new encoding, using varint32 to store size information
    output_header_len = compression::PutDecompressedSizeInfo(
        output, static_cast<uint32_t>(length));
  } else {
    // legacy encoding, which is not really portable (depends on big/little
    // endianness)
    output_header_len = 8;
    output->resize(output_header_len);
    char* p = const_cast<char*>(output->c_str());
    memcpy(p, &length, sizeof(length));
  }
  int compress_bound = LZ4_compressBound(static_cast<int>(length));
  output->resize(static_cast<size_t>(output_header_len + compress_bound));

  int outlen;
#if LZ4_VERSION_NUMBER >= 10400  // r124+
  LZ4_stream_t* stream = LZ4_createStream();
  Slice compression_dict = info.dict().GetRawDict();
  if (compression_dict.size()) {
    LZ4_loadDict(stream, compression_dict.data(),
                 static_cast<int>(compression_dict.size()));
  }
#if LZ4_VERSION_NUMBER >= 10700  // r129+
  int acceleration;
  if (info.options().level < 0) {
    acceleration = -info.options().level;
  } else {
    acceleration = 1;
  }
  outlen = LZ4_compress_fast_continue(
      stream, input, &(*output)[output_header_len], static_cast<int>(length),
      compress_bound, acceleration);
#else  // up to r128
  outlen = LZ4_compress_limitedOutput_continue(
      stream, input, &(*output)[output_header_len], static_cast<int>(length),
      compress_bound);
#endif
  LZ4_freeStream(stream);
#else   // up to r123
  outlen = LZ4_compress_limitedOutput(input, &(*output)[output_header_len],
                                      static_cast<int>(length), compress_bound);
#endif  // LZ4_VERSION_NUMBER >= 10400

  if (outlen == 0) {
    return false;
  }
  output->resize(static_cast<size_t>(output_header_len + outlen));
  return true;
#else  // LZ4
  (void)info;
  (void)compress_format_version;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

// compress_format_version == 1 -- decompressed size is included in the
// block header using memcpy, which makes database non-portable)
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
// @param compression_dict Data for presetting the compression library's
//    dictionary.
inline CacheAllocationPtr LZ4_Uncompress(const UncompressionInfo& info,
                                         const char* input_data,
                                         size_t input_length,
                                         size_t* uncompressed_size,
                                         uint32_t compress_format_version,
                                         MemoryAllocator* allocator = nullptr) {
#ifdef LZ4
  uint32_t output_len = 0;
  if (compress_format_version == 2) {
    // new encoding, using varint32 to store size information
    if (!compression::GetDecompressedSizeInfo(&input_data, &input_length,
                                              &output_len)) {
      return nullptr;
    }
  } else {
    // legacy encoding, which is not really portable (depends on big/little
    // endianness)
    if (input_length < 8) {
      return nullptr;
    }
    if (port::kLittleEndian) {
      memcpy(&output_len, input_data, sizeof(output_len));
    } else {
      memcpy(&output_len, input_data + 4, sizeof(output_len));
    }
    input_length -= 8;
    input_data += 8;
  }

  auto output = AllocateBlock(output_len, allocator);

  int decompress_bytes = 0;

#if LZ4_VERSION_NUMBER >= 10400  // r124+
  LZ4_streamDecode_t* stream = LZ4_createStreamDecode();
  const Slice& compression_dict = info.dict().GetRawDict();
  if (compression_dict.size()) {
    LZ4_setStreamDecode(stream, compression_dict.data(),
                        static_cast<int>(compression_dict.size()));
  }
  decompress_bytes = LZ4_decompress_safe_continue(
      stream, input_data, output.get(), static_cast<int>(input_length),
      static_cast<int>(output_len));
  LZ4_freeStreamDecode(stream);
#else   // up to r123
  decompress_bytes = LZ4_decompress_safe(input_data, output.get(),
                                         static_cast<int>(input_length),
                                         static_cast<int>(output_len));
#endif  // LZ4_VERSION_NUMBER >= 10400

  if (decompress_bytes < 0) {
    return nullptr;
  }
  assert(decompress_bytes == static_cast<int>(output_len));
  *uncompressed_size = decompress_bytes;
  return output;
#else  // LZ4
  (void)info;
  (void)input_data;
  (void)input_length;
  (void)uncompressed_size;
  (void)compress_format_version;
  (void)allocator;
  return nullptr;
#endif
}

// compress_format_version == 1 -- decompressed size is included in the
// block header using memcpy, which makes database non-portable)
// compress_format_version == 2 -- decompressed size is included in the block
// header in varint32 format
// @param compression_dict Data for presetting the compression library's
//    dictionary.
inline bool LZ4HC_Compress(const CompressionInfo& info,
                           uint32_t compress_format_version, const char* input,
                           size_t length, ::std::string* output) {
#ifdef LZ4
  if (length > std::numeric_limits<uint32_t>::max()) {
    // Can't compress more than 4GB
    return false;
  }

  size_t output_header_len = 0;
  if (compress_format_version == 2) {
    // new encoding, using varint32 to store size information
    output_header_len = compression::PutDecompressedSizeInfo(
        output, static_cast<uint32_t>(length));
  } else {
    // legacy encoding, which is not really portable (depends on big/little
    // endianness)
    output_header_len = 8;
    output->resize(output_header_len);
    char* p = const_cast<char*>(output->c_str());
    memcpy(p, &length, sizeof(length));
  }
  int compress_bound = LZ4_compressBound(static_cast<int>(length));
  output->resize(static_cast<size_t>(output_header_len + compress_bound));

  int outlen;
  int level;
  if (info.options().level == CompressionOptions::kDefaultCompressionLevel) {
    level = 0;  // lz4hc.h says any value < 1 will be sanitized to default
  } else {
    level = info.options().level;
  }
#if LZ4_VERSION_NUMBER >= 10400  // r124+
  LZ4_streamHC_t* stream = LZ4_createStreamHC();
  LZ4_resetStreamHC(stream, level);
  Slice compression_dict = info.dict().GetRawDict();
  const char* compression_dict_data =
      compression_dict.size() > 0 ? compression_dict.data() : nullptr;
  size_t compression_dict_size = compression_dict.size();
  if (compression_dict_data != nullptr) {
    LZ4_loadDictHC(stream, compression_dict_data,
                   static_cast<int>(compression_dict_size));
  }

#if LZ4_VERSION_NUMBER >= 10700  // r129+
  outlen =
      LZ4_compress_HC_continue(stream, input, &(*output)[output_header_len],
                               static_cast<int>(length), compress_bound);
#else   // r124-r128
  outlen = LZ4_compressHC_limitedOutput_continue(
      stream, input, &(*output)[output_header_len], static_cast<int>(length),
      compress_bound);
#endif  // LZ4_VERSION_NUMBER >= 10700
  LZ4_freeStreamHC(stream);

#elif LZ4_VERSION_MAJOR  // r113-r123
  outlen = LZ4_compressHC2_limitedOutput(input, &(*output)[output_header_len],
                                         static_cast<int>(length),
                                         compress_bound, level);
#else                    // up to r112
  outlen =
      LZ4_compressHC_limitedOutput(input, &(*output)[output_header_len],
                                   static_cast<int>(length), compress_bound);
#endif                   // LZ4_VERSION_NUMBER >= 10400

  if (outlen == 0) {
    return false;
  }
  output->resize(static_cast<size_t>(output_header_len + outlen));
  return true;
#else  // LZ4
  (void)info;
  (void)compress_format_version;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

#ifdef XPRESS
inline bool XPRESS_Compress(const char* input, size_t length,
                            std::string* output) {
  return port::xpress::Compress(input, length, output);
}
#else
inline bool XPRESS_Compress(const char* /*input*/, size_t /*length*/,
                            std::string* /*output*/) {
  return false;
}
#endif

#ifdef XPRESS
inline char* XPRESS_Uncompress(const char* input_data, size_t input_length,
                               size_t* uncompressed_size) {
  return port::xpress::Decompress(input_data, input_length, uncompressed_size);
}
#else
inline char* XPRESS_Uncompress(const char* /*input_data*/,
                               size_t /*input_length*/,
                               size_t* /*uncompressed_size*/) {
  return nullptr;
}
#endif

inline bool ZSTD_Compress(const CompressionInfo& info, const char* input,
                          size_t length, ::std::string* output) {
#ifdef ZSTD
  if (length > std::numeric_limits<uint32_t>::max()) {
    // Can't compress more than 4GB
    return false;
  }

  size_t output_header_len = compression::PutDecompressedSizeInfo(
      output, static_cast<uint32_t>(length));

  size_t compressBound = ZSTD_compressBound(length);
  // TODO: use resize_and_overwrite with c++23
  output->resize(static_cast<size_t>(output_header_len + compressBound));
  size_t outlen = 0;
  ZSTD_CCtx* context = info.context().ZSTDPreallocCtx();
  assert(context != nullptr);
  if (info.dict().GetDigestedZstdCDict() != nullptr) {
    ZSTD_CCtx_refCDict(context, info.dict().GetDigestedZstdCDict());
  } else {
    ZSTD_CCtx_loadDictionary(context, info.dict().GetRawDict().data(),
                             info.dict().GetRawDict().size());
  }

  // Compression level is set in `contex` during CreateNativeContext()
  outlen = ZSTD_compress2(context, &(*output)[output_header_len], compressBound,
                          input, length);
  if (outlen == 0) {
    return false;
  }
  output->resize(output_header_len + outlen);
  return true;
#else  // ZSTD
  (void)info;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif
}

// @param compression_dict Data for presetting the compression library's
//    dictionary.
// @param error_message If not null, will be set if decompression fails.
//
// Returns nullptr if decompression fails.
inline CacheAllocationPtr ZSTD_Uncompress(
    const UncompressionInfo& info, const char* input_data, size_t input_length,
    size_t* uncompressed_size, MemoryAllocator* allocator = nullptr,
    const char** error_message = nullptr) {
#ifdef ZSTD
  static const char* const kErrorDecodeOutputSize =
      "Cannot decode output size.";
  static const char* const kErrorOutputLenMismatch =
      "Decompressed size does not match header.";
  uint32_t output_len = 0;
  if (!compression::GetDecompressedSizeInfo(&input_data, &input_length,
                                            &output_len)) {
    if (error_message) {
      *error_message = kErrorDecodeOutputSize;
    }
    return nullptr;
  }

  CacheAllocationPtr output = AllocateBlock(output_len, allocator);
  size_t actual_output_length = 0;
  ZSTD_DCtx* context = info.context().GetZSTDContext();
  assert(context != nullptr);
#ifdef ROCKSDB_ZSTD_DDICT
  if (info.dict().GetDigestedZstdDDict() != nullptr) {
    actual_output_length = ZSTD_decompress_usingDDict(
        context, output.get(), output_len, input_data, input_length,
        info.dict().GetDigestedZstdDDict());
  } else {
#endif  // ROCKSDB_ZSTD_DDICT
    actual_output_length = ZSTD_decompress_usingDict(
        context, output.get(), output_len, input_data, input_length,
        info.dict().GetRawDict().data(), info.dict().GetRawDict().size());
#ifdef ROCKSDB_ZSTD_DDICT
  }
#endif  // ROCKSDB_ZSTD_DDICT
  if (ZSTD_isError(actual_output_length)) {
    if (error_message) {
      *error_message = ZSTD_getErrorName(actual_output_length);
    }
    return nullptr;
  } else if (actual_output_length != output_len) {
    if (error_message) {
      *error_message = kErrorOutputLenMismatch;
    }
    return nullptr;
  }

  *uncompressed_size = actual_output_length;
  return output;
#else  // ZSTD
  (void)info;
  (void)input_data;
  (void)input_length;
  (void)uncompressed_size;
  (void)allocator;
  (void)error_message;
  return nullptr;
#endif
}

inline bool ZSTD_TrainDictionarySupported() {
#ifdef ZSTD
  // NB: Dictionary trainer is available since v0.6.1 for static linking, but
  // not available for dynamic linking until v1.1.3. See ZSTD_VERSION_NUMBER
  // check above.
  return true;
#else
  return false;
#endif
}

inline std::string ZSTD_TrainDictionary(const std::string& samples,
                                        const std::vector<size_t>& sample_lens,
                                        size_t max_dict_bytes) {
#ifdef ZSTD
  assert(samples.empty() == sample_lens.empty());
  if (samples.empty()) {
    return "";
  }
  std::string dict_data(max_dict_bytes, '\0');
  size_t dict_len = ZDICT_trainFromBuffer(
      &dict_data[0], max_dict_bytes, &samples[0], &sample_lens[0],
      static_cast<unsigned>(sample_lens.size()));
  if (ZDICT_isError(dict_len)) {
    return "";
  }
  assert(dict_len <= max_dict_bytes);
  dict_data.resize(dict_len);
  return dict_data;
#else
  assert(false);
  (void)samples;
  (void)sample_lens;
  (void)max_dict_bytes;
  return "";
#endif  // ZSTD
}

inline std::string ZSTD_TrainDictionary(const std::string& samples,
                                        size_t sample_len_shift,
                                        size_t max_dict_bytes) {
#ifdef ZSTD
  // skips potential partial sample at the end of "samples"
  size_t num_samples = samples.size() >> sample_len_shift;
  std::vector<size_t> sample_lens(num_samples, size_t(1) << sample_len_shift);
  return ZSTD_TrainDictionary(samples, sample_lens, max_dict_bytes);
#else
  assert(false);
  (void)samples;
  (void)sample_len_shift;
  (void)max_dict_bytes;
  return "";
#endif  // ZSTD
}

inline bool ZSTD_FinalizeDictionarySupported() {
#ifdef ROCKSDB_ZDICT_FINALIZE
  return true;
#else
  return false;
#endif
}

inline std::string ZSTD_FinalizeDictionary(
    const std::string& samples, const std::vector<size_t>& sample_lens,
    size_t max_dict_bytes, int level) {
#ifdef ROCKSDB_ZDICT_FINALIZE
  assert(samples.empty() == sample_lens.empty());
  if (samples.empty()) {
    return "";
  }
  if (level == CompressionOptions::kDefaultCompressionLevel) {
    // NB: ZSTD_CLEVEL_DEFAULT is historically == 3
    level = ZSTD_CLEVEL_DEFAULT;
  }
  std::string dict_data(max_dict_bytes, '\0');
  size_t dict_len = ZDICT_finalizeDictionary(
      dict_data.data(), max_dict_bytes, samples.data(),
      std::min(static_cast<size_t>(samples.size()), max_dict_bytes),
      samples.data(), sample_lens.data(),
      static_cast<unsigned>(sample_lens.size()),
      {level, 0 /* notificationLevel */, 0 /* dictID */});
  if (ZDICT_isError(dict_len)) {
    return "";
  } else {
    assert(dict_len <= max_dict_bytes);
    dict_data.resize(dict_len);
    return dict_data;
  }
#else
  assert(false);
  (void)samples;
  (void)sample_lens;
  (void)max_dict_bytes;
  (void)level;
  return "";
#endif  // ROCKSDB_ZDICT_FINALIZE
}

inline bool OLD_CompressData(const Slice& raw,
                             const CompressionInfo& compression_info,
                             uint32_t compress_format_version,
                             std::string* compressed_output) {
  bool ret = false;

  // Will return compressed block contents if (1) the compression method is
  // supported in this platform and (2) the compression rate is "good enough".
  switch (compression_info.type()) {
    case kSnappyCompression:
      ret = Snappy_Compress(compression_info, raw.data(), raw.size(),
                            compressed_output);
      break;
    case kZlibCompression:
      ret = Zlib_Compress(compression_info, compress_format_version, raw.data(),
                          raw.size(), compressed_output);
      break;
    case kBZip2Compression:
      ret = BZip2_Compress(compression_info, compress_format_version,
                           raw.data(), raw.size(), compressed_output);
      break;
    case kLZ4Compression:
      ret = LZ4_Compress(compression_info, compress_format_version, raw.data(),
                         raw.size(), compressed_output);
      break;
    case kLZ4HCCompression:
      ret = LZ4HC_Compress(compression_info, compress_format_version,
                           raw.data(), raw.size(), compressed_output);
      break;
    case kXpressCompression:
      ret = XPRESS_Compress(raw.data(), raw.size(), compressed_output);
      break;
    case kZSTD:
      ret = ZSTD_Compress(compression_info, raw.data(), raw.size(),
                          compressed_output);
      break;
    default:
      // Do not recognize this compression type
      break;
  }

  TEST_SYNC_POINT_CALLBACK("CompressData:TamperWithReturnValue",
                           static_cast<void*>(&ret));

  return ret;
}

inline CacheAllocationPtr OLD_UncompressData(
    const UncompressionInfo& uncompression_info, const char* data, size_t n,
    size_t* uncompressed_size, uint32_t compress_format_version,
    MemoryAllocator* allocator = nullptr,
    const char** error_message = nullptr) {
  switch (uncompression_info.type()) {
    case kSnappyCompression:
      return Snappy_Uncompress(data, n, uncompressed_size, allocator);
    case kZlibCompression:
      return Zlib_Uncompress(uncompression_info, data, n, uncompressed_size,
                             compress_format_version, allocator);
    case kBZip2Compression:
      return BZip2_Uncompress(data, n, uncompressed_size,
                              compress_format_version, allocator);
    case kLZ4Compression:
    case kLZ4HCCompression:
      return LZ4_Uncompress(uncompression_info, data, n, uncompressed_size,
                            compress_format_version, allocator);
    case kXpressCompression:
      // XPRESS allocates memory internally, thus no support for custom
      // allocator.
      return CacheAllocationPtr(XPRESS_Uncompress(data, n, uncompressed_size));
    case kZSTD:
      // TODO(cbi): error message handling for other compression algorithms.
      return ZSTD_Uncompress(uncompression_info, data, n, uncompressed_size,
                             allocator, error_message);
    default:
      return CacheAllocationPtr();
  }
}

// ***********************************************************************
// BEGIN built-in implementation of customization interface
// ***********************************************************************

// NOTE: to avoid compression API depending on block-based table API, uses
// its own format version. See internal function GetCompressFormatForVersion()
const std::shared_ptr<CompressionManager>& GetBuiltinCompressionManager(
    int compression_format_version);

// ***********************************************************************
// END built-in implementation of customization interface
// ***********************************************************************

// Records the compression type for subsequent WAL records.
class CompressionTypeRecord {
 public:
  explicit CompressionTypeRecord(CompressionType compression_type)
      : compression_type_(compression_type) {}

  CompressionType GetCompressionType() const { return compression_type_; }

  inline void EncodeTo(std::string* dst) const {
    assert(dst != nullptr);
    PutFixed32(dst, compression_type_);
  }

  inline Status DecodeFrom(Slice* src) {
    constexpr char class_name[] = "CompressionTypeRecord";

    uint32_t val;
    if (!GetFixed32(src, &val)) {
      return Status::Corruption(class_name,
                                "Error decoding WAL compression type");
    }
    CompressionType compression_type = static_cast<CompressionType>(val);
    if (!StreamingCompressionTypeSupported(compression_type)) {
      return Status::Corruption(class_name,
                                "WAL compression type not supported");
    }
    compression_type_ = compression_type;
    return Status::OK();
  }

  inline std::string DebugString() const {
    return "compression_type: " + CompressionTypeToString(compression_type_);
  }

 private:
  CompressionType compression_type_;
};

// Base class to implement compression for a stream of buffers.
// Instantiate an implementation of the class using Create() with the
// compression type and use Compress() repeatedly.
// The output buffer needs to be at least max_output_len.
// Call Reset() in between frame boundaries or in case of an error.
// NOTE: This class is not thread safe.
class StreamingCompress {
 public:
  StreamingCompress(CompressionType compression_type,
                    const CompressionOptions& opts,
                    uint32_t compress_format_version, size_t max_output_len)
      : compression_type_(compression_type),
        opts_(opts),
        compress_format_version_(compress_format_version),
        max_output_len_(max_output_len) {}
  virtual ~StreamingCompress() = default;
  // compress should be called repeatedly with the same input till the method
  // returns 0
  // Parameters:
  // input - buffer to compress
  // input_size - size of input buffer
  // output - compressed buffer allocated by caller, should be at least
  // max_output_len
  // output_size - size of the output buffer
  // Returns -1 for errors, the remaining size of the input buffer that needs
  // to be compressed
  virtual int Compress(const char* input, size_t input_size, char* output,
                       size_t* output_pos) = 0;
  // static method to create object of a class inherited from
  // StreamingCompress based on the actual compression type.
  static StreamingCompress* Create(CompressionType compression_type,
                                   const CompressionOptions& opts,
                                   uint32_t compress_format_version,
                                   size_t max_output_len);
  virtual void Reset() = 0;

 protected:
  const CompressionType compression_type_;
  const CompressionOptions opts_;
  const uint32_t compress_format_version_;
  const size_t max_output_len_;
};

// Base class to uncompress a stream of compressed buffers.
// Instantiate an implementation of the class using Create() with the
// compression type and use Uncompress() repeatedly.
// The output buffer needs to be at least max_output_len.
// Call Reset() in between frame boundaries or in case of an error.
// NOTE: This class is not thread safe.
class StreamingUncompress {
 public:
  StreamingUncompress(CompressionType compression_type,
                      uint32_t compress_format_version, size_t max_output_len)
      : compression_type_(compression_type),
        compress_format_version_(compress_format_version),
        max_output_len_(max_output_len) {}
  virtual ~StreamingUncompress() = default;
  // Uncompress can be called repeatedly to progressively process the same
  // input buffer, or can be called with a new input buffer. When the input
  // buffer is not fully consumed, the return value is > 0 or output_size
  // == max_output_len. When calling uncompress to continue processing the
  // same input buffer, the input argument should be nullptr.
  // Parameters:
  // input - buffer to uncompress
  // input_size - size of input buffer
  // output - uncompressed buffer allocated by caller, should be at least
  // max_output_len
  // output_size - size of the output buffer
  // Returns -1 for errors, remaining input to be processed otherwise.
  virtual int Uncompress(const char* input, size_t input_size, char* output,
                         size_t* output_pos) = 0;
  static StreamingUncompress* Create(CompressionType compression_type,
                                     uint32_t compress_format_version,
                                     size_t max_output_len);
  virtual void Reset() = 0;

 protected:
  CompressionType compression_type_;
  uint32_t compress_format_version_;
  size_t max_output_len_;
};

class ZSTDStreamingCompress final : public StreamingCompress {
 public:
  explicit ZSTDStreamingCompress(const CompressionOptions& opts,
                                 uint32_t compress_format_version,
                                 size_t max_output_len)
      : StreamingCompress(kZSTD, opts, compress_format_version,
                          max_output_len) {
#ifdef ZSTD
    cctx_ = ZSTD_createCCtx();
    // Each compressed frame will have a checksum
    ZSTD_CCtx_setParameter(cctx_, ZSTD_c_checksumFlag, 1);
    assert(cctx_ != nullptr);
    input_buffer_ = {/*src=*/nullptr, /*size=*/0, /*pos=*/0};
#endif
  }
  ~ZSTDStreamingCompress() override {
#ifdef ZSTD
    ZSTD_freeCCtx(cctx_);
#endif
  }
  int Compress(const char* input, size_t input_size, char* output,
               size_t* output_pos) override;
  void Reset() override;
#ifdef ZSTD
  ZSTD_CCtx* cctx_;
  ZSTD_inBuffer input_buffer_;
#endif
};

class ZSTDStreamingUncompress final : public StreamingUncompress {
 public:
  explicit ZSTDStreamingUncompress(uint32_t compress_format_version,
                                   size_t max_output_len)
      : StreamingUncompress(kZSTD, compress_format_version, max_output_len) {
#ifdef ZSTD
    dctx_ = ZSTD_createDCtx();
    assert(dctx_ != nullptr);
    input_buffer_ = {/*src=*/nullptr, /*size=*/0, /*pos=*/0};
#endif
  }
  ~ZSTDStreamingUncompress() override {
#ifdef ZSTD
    ZSTD_freeDCtx(dctx_);
#endif
  }
  int Uncompress(const char* input, size_t input_size, char* output,
                 size_t* output_size) override;
  void Reset() override;

 private:
#ifdef ZSTD
  ZSTD_DCtx* dctx_;
  ZSTD_inBuffer input_buffer_;
#endif
};

}  // namespace ROCKSDB_NAMESPACE
