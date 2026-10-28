/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_parquet_reader.h"
#include <cstring>
#include "lib/ob_errno.h"

namespace oceanbase
{
namespace sql
{

namespace
{

// Compact protocol constants.
enum
{
  PQ_T_STOP = 0,
  PQ_T_BOOL_TRUE = 1,
  PQ_T_BOOL_FALSE = 2,
  PQ_T_BYTE = 3,
  PQ_T_I16 = 4,
  PQ_T_I32 = 5,
  PQ_T_I64 = 6,
  PQ_T_DOUBLE = 7,
  PQ_T_BINARY = 8,
  PQ_T_LIST = 9,
  PQ_T_SET = 10,
  PQ_T_MAP = 11,
  PQ_T_STRUCT = 12,
};

// Thrift Type enum values used by parquet (physical types).
// Thrift parquet Type enum (physical types).
enum
{
  PQ_PT_BOOLEAN = 0,
  PQ_PT_INT32 = 1,
  PQ_PT_INT64 = 2,
  PQ_PT_DOUBLE = 5,
  PQ_PT_BYTE_ARRAY = 6,
};

struct PqCursor
{
  const char *buf_;
  int64_t len_;
  int64_t pos_;
  int16_t last_fid_;

  PqCursor(const char *buf, int64_t len) : buf_(buf), len_(len), pos_(0), last_fid_(0) {}

  // Compact protocol: the field-id counter is per struct; nested structs and
  // list elements restart at 0 and restore the enclosing counter afterwards.
  int16_t enter_struct()
  {
    const int16_t saved = last_fid_;
    last_fid_ = 0;
    return saved;
  }

  void leave_struct(int16_t saved) { last_fid_ = saved; }

  bool ok() const { return pos_ <= len_; }

  int64_t uvarint()
  {
    int64_t v = 0;
    int32_t shift = 0;
    while (pos_ < len_) {
      const uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
      v |= static_cast<int64_t>(b & 0x7F) << shift;
      if (0 == (b & 0x80)) {
        break;
      }
      shift += 7;
    }
    return v;
  }

  int64_t zigzag(int64_t v) { return (v >> 1) ^ -(v & 1); }

  // Returns 0 on stop, 1 on a field, -1 on malformed input.
  int field_header(int16_t &fid, int8_t &ftype)
  {
    if (pos_ >= len_) {
      return -1;
    }
    const uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
    if (0 == b) {
      return 0;
    }
    ftype = static_cast<int8_t>(b & 0x0F);
    const int16_t delta = static_cast<int16_t>((b >> 4) & 0x0F);
    if (0 == delta) {
      last_fid_ = static_cast<int16_t>(zigzag(uvarint()));
    } else {
      last_fid_ = static_cast<int16_t>(last_fid_ + delta);
    }
    fid = last_fid_;
    return 1;
  }

  int skip_value(int8_t ftype)
  {
    int ret = OB_SUCCESS;
    switch (ftype) {
      case PQ_T_BOOL_TRUE:
      case PQ_T_BOOL_FALSE:
        break;
      case PQ_T_BYTE:
        pos_ += 1;
        break;
      case PQ_T_I16:
        pos_ += 2;
        break;
      case PQ_T_I32:
      case PQ_T_I64:
        (void)uvarint();
        break;
      case PQ_T_DOUBLE:
        pos_ += 8;
        break;
      case PQ_T_BINARY: {
        const int64_t n = uvarint();
        pos_ += n;
        break;
      }
      case PQ_T_STRUCT: {
        int16_t fid = 0;
        int8_t ft = 0;
        int fr = 0;
        const int16_t saved = enter_struct();
        while (1 == (fr = field_header(fid, ft))) {
          if (OB_FAIL(skip_value(ft))) {
            break;
          }
        }
        leave_struct(saved);
        if (fr < 0) {
          ret = OB_ERR_UNEXPECTED;
        }
        break;
      }
      case PQ_T_LIST:
      case PQ_T_SET:
      case PQ_T_MAP: {
        if (pos_ >= len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          uint8_t hdr = static_cast<uint8_t>(buf_[pos_++]);
          int64_t count = hdr >> 4;
          int8_t elem_t = static_cast<int8_t>(hdr & 0x0F);
          if (15 == count) {
            count = uvarint();
          }
          if (PQ_T_MAP == ftype) {
            pos_ += 1; // key type byte
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            if (PQ_T_STRUCT == elem_t) {
              const int16_t saved = enter_struct();
              if (OB_FAIL(skip_value(elem_t))) {
              }
              leave_struct(saved);
            } else if (OB_FAIL(skip_value(elem_t))) {
            }
          }
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
    return ret;
  }
};

int parse_footer(const char *data, int64_t len, ObParquetFileInfo &info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(data) || len < 12) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 != MEMCMP(data, "PAR1", 4) || 0 != MEMCMP(data + len - 4, "PAR1", 4)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    uint32_t footer_len = 0;
    MEMCPY(&footer_len, data + len - 8, 4);
    const int64_t meta_start = len - 8 - static_cast<int64_t>(footer_len);
    if (meta_start < 4 || meta_start + footer_len > len - 8) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      PqCursor cur(data + meta_start, footer_len);
      int16_t fid = 0;
      int8_t ft = 0;
      int fr = 0;
      int32_t schema_leaf_type = 0;
      bool schema_seen = false;
      common::ObString col_name;
      int32_t codec = 0;
      int64_t num_values = 0;
      int64_t data_page_offset = 0;
      int64_t total_compressed_size = 0;
      while (OB_SUCC(ret) && 1 == (fr = cur.field_header(fid, ft))) {
        if (2 == fid && PQ_T_LIST == ft) {
          // schema: list<SchemaElement>; first element is the root struct,
          // the second is the leaf column
          if (cur.pos_ >= cur.len_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const uint8_t hdr = static_cast<uint8_t>(cur.buf_[cur.pos_++]);
            const int64_t count = hdr >> 4;
            const int8_t elem_t = static_cast<int8_t>(hdr & 0x0F);
            if (count < 1 || PQ_T_STRUCT != elem_t) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
                // i == 0: root; i == 1: the leaf column. Match by element
                // position, not by field id.
                int16_t sfid = 0;
                int8_t sft = 0;
                int sfr = 0;
                bool got_type = false;
                const int16_t schema_saved = cur.enter_struct();
                while (OB_SUCC(ret) && 1 == (sfr = cur.field_header(sfid, sft))) {
                  if (PQ_T_I32 == sft && 1 == i && !got_type) {
                    // the leaf's SchemaElement.type
                    schema_leaf_type = static_cast<int32_t>(cur.zigzag(cur.uvarint()));
                    got_type = true;
                  } else if (PQ_T_BINARY == sft && 1 == i) {
                    const int64_t n = cur.uvarint();
                    if (n >= 0 && cur.pos_ + n <= cur.len_) {
                      col_name.assign_ptr(cur.buf_ + cur.pos_, static_cast<int32_t>(n));
                      cur.pos_ += n;
                    } else {
                      ret = OB_ERR_UNEXPECTED;
                    }
                  } else if (OB_FAIL(cur.skip_value(sft))) {
                  }
                }
                if (sfr < 0) {
                  ret = OB_ERR_UNEXPECTED;
                }
                cur.leave_struct(schema_saved);
              }
            }
          }
          schema_seen = true;
        } else if (4 == fid && PQ_T_LIST == ft) {
          // row_groups: list<RowGroup>; RowGroup field 1 = columns
          if (cur.pos_ >= cur.len_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const uint8_t hdr = static_cast<uint8_t>(cur.buf_[cur.pos_++]);
            const int64_t group_cnt = hdr >> 4;
            const int8_t g_elem = static_cast<int8_t>(hdr & 0x0F);
            if (PQ_T_STRUCT != g_elem) {
              ret = OB_ERR_UNEXPECTED;
            }
            for (int64_t g = 0; OB_SUCC(ret) && g < group_cnt; ++g) {
              int16_t gfid = 0;
              int8_t gft = 0;
              int gfr = 0;
              const int16_t group_saved = cur.enter_struct();
              while (OB_SUCC(ret) && 1 == (gfr = cur.field_header(gfid, gft))) {
                if (1 == gfid && PQ_T_LIST == gft) {
                  // columns: list<ColumnChunk>
                  if (cur.pos_ >= cur.len_) {
                    ret = OB_ERR_UNEXPECTED;
                  } else {
                    const uint8_t chdr = static_cast<uint8_t>(cur.buf_[cur.pos_++]);
                    const int64_t col_cnt = chdr >> 4;
                    const int8_t c_elem = static_cast<int8_t>(chdr & 0x0F);
                    if (PQ_T_STRUCT != c_elem) {
                      ret = OB_ERR_UNEXPECTED;
                    }
                    for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
                      int16_t cfid = 0;
                      int8_t cft = 0;
                      int cfr = 0;
                      const int16_t col_saved = cur.enter_struct();
                      while (OB_SUCC(ret) && 1 == (cfr = cur.field_header(cfid, cft))) {
                        if (3 == cfid && PQ_T_STRUCT == cft) {
                          // ColumnMetaData
                          int16_t mfid = 0;
                          int8_t mft = 0;
                          int mfr = 0;
                          const int16_t meta_saved = cur.enter_struct();
                          while (OB_SUCC(ret) && 1 == (mfr = cur.field_header(mfid, mft))) {
                            if (4 == mfid && PQ_T_I32 == mft) {
                              codec = static_cast<int32_t>(cur.zigzag(cur.uvarint()));
                            } else if (5 == mfid && PQ_T_I64 == mft) {
                              num_values = cur.zigzag(cur.uvarint());
                            } else if (7 == mfid && PQ_T_I64 == mft) {
                              total_compressed_size = cur.zigzag(cur.uvarint());
                            } else if (9 == mfid && PQ_T_I64 == mft) {
                              data_page_offset = cur.zigzag(cur.uvarint());
                            } else if (OB_FAIL(cur.skip_value(mft))) {
                            }
                          }
                          cur.leave_struct(meta_saved);
                          if (mfr < 0) {
                            ret = OB_ERR_UNEXPECTED;
                          }
                        } else if (OB_FAIL(cur.skip_value(cft))) {
                        }
                      }
                      cur.leave_struct(col_saved);
                      if (cfr < 0) {
                        ret = OB_ERR_UNEXPECTED;
                      }
                    }
                  }
                } else if (2 == gfid || 3 == gfid) {
                  if (OB_FAIL(cur.skip_value(gft))) {
                  }
                } else if (OB_FAIL(cur.skip_value(gft))) {
                }
              }
              cur.leave_struct(group_saved);
              if (gfr < 0) {
                ret = OB_ERR_UNEXPECTED;
              }
            }
          }
        } else if (OB_FAIL(cur.skip_value(ft))) {
        }
      }
      if (fr < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ret)) {
      } else if (!schema_seen || col_name.empty()) {
        ret = OB_NOT_SUPPORTED;
      } else {
        info.valid_ = true;
        info.col_name_ = col_name;
        info.physical_type_ = schema_leaf_type;
        info.codec_ = codec;
        info.num_values_ = num_values;
        info.data_page_offset_ = data_page_offset;
        info.total_compressed_size_ = total_compressed_size;
      }
    }
  }
  return ret;
}

int parse_data_page(const char *data, int64_t len, const ObParquetFileInfo &info,
                    int64_t &value_offset, int64_t &value_bytes)
{
  int ret = OB_SUCCESS;
  if (!info.valid_ || info.data_page_offset_ < 4 || info.data_page_offset_ >= len) {
    ret = OB_NOT_SUPPORTED;
  } else {
    PqCursor cur(data + info.data_page_offset_, len - info.data_page_offset_);
    int16_t fid = 0;
    int8_t ft = 0;
    int fr = 0;
    while (OB_SUCC(ret) && 1 == (fr = cur.field_header(fid, ft))) {
      if (1 == fid && PQ_T_I32 == ft) {
        (void)cur.zigzag(cur.uvarint()); // page type (0 = DATA_PAGE)
      } else if (2 == fid && PQ_T_I32 == ft) {
        (void)cur.zigzag(cur.uvarint()); // uncompressed_page_size
      } else if (3 == fid && PQ_T_I32 == ft) {
        (void)cur.zigzag(cur.uvarint()); // compressed_page_size
      } else if (5 == fid && PQ_T_STRUCT == ft) {
        // DataPageHeader: 1 num_values, 2 encoding, 3 def encoding, 4 rep encoding
        int16_t dfid = 0;
        int8_t dft = 0;
        int dfr = 0;
        while (OB_SUCC(ret) && 1 == (dfr = cur.field_header(dfid, dft))) {
          if (OB_FAIL(cur.skip_value(dft))) {
          }
        }
        if (dfr < 0) {
          ret = OB_ERR_UNEXPECTED;
        }
      } else if (OB_FAIL(cur.skip_value(ft))) {
      }
    }
    if (fr < 0) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ret)) {
    } else {
      // DATA_PAGE_V2 layout: [header][rep levels][def levels][values]; the
      // values are the trailing bytes of the column chunk. Compute the value
      // offset from the chunk end to stay robust against header variance.
      const int64_t width = (1 == info.physical_type_) ? 4 : 8; // INT32 vs others
      value_bytes = info.num_values_ * width;
      if (info.total_compressed_size_ <= 0) {
        value_offset = info.data_page_offset_ + cur.pos_;
      } else {
        value_offset = info.data_page_offset_ + info.total_compressed_size_ - value_bytes;
      }
      if (value_offset < 4 || value_offset + value_bytes > len) {
        ret = OB_FILE_NOT_EXIST;
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ob_parse_parquet_footer(const char *data, int64_t len, ObParquetFileInfo &info)
{
  int ret = OB_SUCCESS;
  info.reset();
  if (OB_FAIL(parse_footer(data, len, info))) {
    LOG_WARN("fail to parse parquet footer", K(ret));
  } else if (!info.valid_ || info.codec_ > 1) {
    ret = OB_NOT_SUPPORTED;
  } else {
    int64_t value_offset = 0;
    int64_t value_bytes = 0;
    if (OB_FAIL(parse_data_page(data, len, info, value_offset, value_bytes))) {
      LOG_WARN("fail to parse parquet data page", K(ret));
    } else {
      info.value_data_offset_ = value_offset;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
