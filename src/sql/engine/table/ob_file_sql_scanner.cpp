/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/table/ob_file_sql_scanner.h"
#include "lib/charset/ob_charset.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/ob_errno.h"
#include <cstring>
#include <cstdlib>

namespace oceanbase
{
namespace sql
{

static const char *skip_ws(const char *p, const char *end)
{
  while (p < end && (' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)) {
    ++p;
  }
  return p;
}

static ObString trim_ws(const char *s, const int32_t len)
{
  int32_t begin = 0;
  int32_t end = len;
  while (begin < end && (' ' == s[begin] || '\t' == s[begin])) {
    ++begin;
  }
  while (end > begin && (' ' == s[end - 1] || '\t' == s[end - 1])) {
    --end;
  }
  return ObString(end - begin, s + begin);
}

ObFileSqlScanner::ObFileSqlScanner(const ObFileSqlDef &def)
    : def_(def),
      fp_(NULL),
      buf_(NULL),
      buf_len_(0),
      buf_pos_(0),
      eof_(false),
      has_push_back_(false),
      push_back_('\0'),
      header_skipped_(false),
      scratch_alloc_("FileSqlScanner"),
      scratch_fields_(),
      json_fields_()
{
  file_path_[0] = '\0';
}

ObFileSqlScanner::~ObFileSqlScanner()
{
  close();
}

int ObFileSqlScanner::open()
{
  int ret = OB_SUCCESS;
  close();
  if (ObFileSqlFormat::CSV != def_.format_ && ObFileSqlFormat::JSON != def_.format_) {
    // PARQUET/GLOB scanners are not implemented yet.
    ret = OB_NOT_SUPPORTED;
    LOG_ERROR("file sql scanner only supports csv and jsonl files", K(ret),
             K(static_cast<int8_t>(def_.format_)));
  } else if (def_.path_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_ERROR("file sql path is empty", K(ret));
  } else if (def_.path_.length() >= MAX_FILE_PATH_LEN) {
    ret = OB_NOT_SUPPORTED;
    LOG_ERROR("file sql path is too long", K(ret), K(def_.path_.length()));
  } else {
    MEMCPY(file_path_, def_.path_.ptr(), def_.path_.length());
    file_path_[def_.path_.length()] = '\0';
    fp_ = ::fopen(file_path_, "rb");
    if (OB_ISNULL(fp_)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_ERROR("failed to open file sql source file", K(ret), K(file_path_));
    } else if (OB_ISNULL(buf_ = static_cast<char*>(ob_malloc(IO_BUF_SIZE, "FileSqlScanIO")))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      ::fclose(fp_);
      fp_ = NULL;
    } else {
      buf_len_ = 0;
      buf_pos_ = 0;
      eof_ = false;
      has_push_back_ = false;
      push_back_ = '\0';
      header_skipped_ = !def_.header_;
      if (ObFileSqlFormat::CSV == def_.format_ && def_.header_) {
        skip_csv_header();
      }
    }
  }
  return ret;
}

void ObFileSqlScanner::close()
{
  if (OB_NOT_NULL(fp_)) {
    ::fclose(fp_);
    fp_ = NULL;
  }
  if (OB_NOT_NULL(buf_)) {
    ob_free(buf_);
    buf_ = NULL;
  }
  buf_len_ = 0;
  buf_pos_ = 0;
  eof_ = false;
  has_push_back_ = false;
  push_back_ = '\0';
  header_skipped_ = false;
  scratch_alloc_.reset();
  scratch_fields_.reuse();
  json_fields_.reuse();
}

int ObFileSqlScanner::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(fp_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file sql scanner is not opened", K(ret));
  } else {
    ::clearerr(fp_);
    if (0 != ::fseek(fp_, 0, SEEK_SET)) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to rewind file sql source file", K(ret), K(file_path_));
    } else {
      buf_len_ = 0;
      buf_pos_ = 0;
      eof_ = false;
      has_push_back_ = false;
      push_back_ = '\0';
      header_skipped_ = !def_.header_;
      scratch_alloc_.reuse();
      scratch_fields_.reuse();
      json_fields_.reuse();
      if (ObFileSqlFormat::CSV == def_.format_ && def_.header_) {
        skip_csv_header();
      }
    }
  }
  return ret;
}

int ObFileSqlScanner::fill_buf()
{
  int ret = OB_SUCCESS;
  if (eof_) {
    ret = OB_ITER_END;
  } else {
    const size_t n = ::fread(buf_, 1, IO_BUF_SIZE, fp_);
    if (0 == n) {
      if (::ferror(fp_)) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read file sql source file", K(ret), K(file_path_));
      } else {
        eof_ = true;
        ret = OB_ITER_END;
      }
    } else {
      buf_len_ = static_cast<int64_t>(n);
      buf_pos_ = 0;
    }
  }
  return ret;
}

int ObFileSqlScanner::read_char(char &c)
{
  int ret = OB_SUCCESS;
  if (has_push_back_) {
    c = push_back_;
    has_push_back_ = false;
  } else if (buf_pos_ >= buf_len_) {
    if (OB_FAIL(fill_buf())) {
      // OB_ITER_END at eof
    } else {
      c = buf_[buf_pos_++];
    }
  } else {
    c = buf_[buf_pos_++];
  }
  return ret;
}

int ObFileSqlScanner::push_field(common::ObSEArray<ObString, 32> &fields,
                                 const ObStringBuffer &field)
{
  int ret = OB_SUCCESS;
  // deep-copy: the field buffer is reused for the next field, so the
  // pushed string must not alias it
  ObString copied;
  if (OB_FAIL(ob_write_string(scratch_alloc_, field.string(), copied))) {
    LOG_WARN("failed to deep copy csv field", K(ret));
  } else if (OB_FAIL(fields.push_back(copied))) {
    LOG_WARN("failed to push csv field", K(ret));
  }
  return ret;
}

void ObFileSqlScanner::skip_csv_header()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(read_csv_record(scratch_fields_))) {
    if (OB_ITER_END == ret) {
      // empty file: nothing to skip
      header_skipped_ = true;
    }
  } else {
    header_skipped_ = true;
  }
  scratch_fields_.reuse();
}

// Reads one CSV record into %fields. Returns OB_SUCCESS with a record,
// OB_ITER_END on clean EOF (no record), or an error code. Blank lines are
// skipped. A record may span lines when a quoted field contains newlines.
int ObFileSqlScanner::read_csv_record(common::ObSEArray<ObString, 32> &fields)
{
  int ret = OB_SUCCESS;
  for (;;) {
    fields.reuse();
    scratch_alloc_.reuse();
    ObStringBuffer field(&scratch_alloc_);
    bool in_quotes = false;
    bool any_field = false;
    bool has_content = false;
    char c = '\0';
    bool record_done = false;
    while (OB_SUCC(ret) && !record_done) {
      const int tmp_ret = read_char(c);
      if (OB_ITER_END == tmp_ret) {
        // EOF in the middle of a record: the trailing record is still valid
        // unless nothing at all was seen for it.
        if (has_content || any_field || in_quotes) {
          ret = OB_SUCCESS;
          record_done = true;
        } else {
          ret = OB_ITER_END;
        }
      } else if (OB_FAIL(tmp_ret)) {
        // error stays
      } else if (in_quotes) {
        has_content = true;
        if (def_.escape_ != '\0' && c == def_.escape_ && c != def_.quote_) {
          // custom escape: take next char literally
          if (OB_FAIL(read_char(c))) {
            if (OB_ITER_END == ret) {
              ret = OB_ERR_UNEXPECTED;
            }
          } else if (OB_FAIL(field.append(&c, 1))) {
          }
        } else if (c == def_.quote_) {
          char next = '\0';
          const int tmp = read_char(next);
          if (OB_ITER_END == tmp) {
            in_quotes = false; // unterminated quoted field at EOF: accept as-is
          } else if (OB_FAIL(tmp)) {
          } else if (next == def_.quote_) {
            // doubled quote -> literal quote
            if (OB_FAIL(field.append(&next, 1))) {
            }
          } else {
            in_quotes = false;
            push_back_ = next;
            has_push_back_ = true;
          }
        } else if (OB_FAIL(field.append(&c, 1))) {
        }
      } else if (c == def_.quote_ && field.length() == 0) {
        in_quotes = true;
        has_content = true;
      } else if (c == def_.delim_) {
        any_field = true;
        if (OB_FAIL(push_field(fields, field))) {
        } else {
          field.reuse();
        }
      } else if (c == '\n') {
        record_done = true;
      } else if (c == '\r') {
        // CRLF or lone CR: consume \n if it follows, then end the record
        char next = '\0';
        const int tmp = read_char(next);
        if (OB_ITER_END == tmp) {
          record_done = true;
        } else if (OB_FAIL(tmp)) {
        } else {
          if (next != '\n') {
            push_back_ = next;
            has_push_back_ = true;
          }
          record_done = true;
        }
      } else {
        has_content = true;
        if (OB_FAIL(field.append(&c, 1))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
      break;
    }
    if (OB_FAIL(push_field(fields, field))) {
      break;
    }
    if (fields.count() == 1 && fields.at(0).empty() && !has_content && !any_field) {
      // blank line: skip it
      continue;
    }
    break;
  }
  return ret;
}

// Reads one JSONL line into %line (a slice of the scratch arena).
// Returns OB_ITER_END on clean EOF.
int ObFileSqlScanner::read_json_line(ObString &line)
{
  int ret = OB_SUCCESS;
  scratch_alloc_.reuse();
  ObStringBuffer buf(&scratch_alloc_);
  bool in_string = false;
  bool escaped = false;
  bool any = false;
  char c = '\0';
  while (OB_SUCC(ret)) {
    const int tmp = read_char(c);
    if (OB_ITER_END == tmp) {
      if (!any) {
        ret = OB_ITER_END;
      }
      break;
    } else if (OB_FAIL(tmp)) {
      break;
    }
    any = true;
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if ('\\' == c) {
        escaped = true;
      } else if ('"' == c) {
        in_string = false;
      }
    } else if ('"' == c) {
      in_string = true;
    } else if ('\n' == c) {
      break;
    }
    if (OB_FAIL(buf.append(&c, 1))) {
    }
  }
  if (OB_SUCC(ret)) {
    line = ObString(static_cast<int32_t>(buf.length()), buf.ptr());
    // strip trailing CR of CRLF
    if (line.length() > 0 && '\r' == line.ptr()[line.length() - 1]) {
      line = ObString(line.length() - 1, line.ptr());
    }
  }
  return ret;
}

// Parses one flat JSON object. Values that are themselves objects or arrays
// are rejected with OB_ERR_UNEXPECTED (nested data is not supported).
int ObFileSqlScanner::parse_json_object(const ObString &line,
                                        common::ObSEArray<ObFileSqlJsonField, 32> &fields)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  const char *end = line.ptr() + line.length();
  const char *cur = skip_ws(line.ptr(), end);
  if (cur >= end || '{' != *cur) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("jsonl row is not a json object", K(ret), K(line));
  } else {
    ++cur;
    cur = skip_ws(cur, end);
    if (cur < end && '}' == *cur) {
      cur = skip_ws(cur + 1, end);
    } else {
      while (OB_SUCC(ret) && cur < end) {
        if ('"' != *cur) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        ++cur;
        const char *key_start = cur;
        while (cur < end && '"' != *cur) {
          if ('\\' == *cur && cur + 1 < end) {
            ++cur;
          }
          ++cur;
        }
        if (cur >= end) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        ObFileSqlJsonField f;
        f.name_ = ObString(static_cast<int32_t>(cur - key_start), key_start);
        ++cur; // closing quote of the key
        cur = skip_ws(cur, end);
        if (cur >= end || ':' != *cur) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        ++cur;
        cur = skip_ws(cur, end);
        if (cur >= end) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        if ('{' == *cur || '[' == *cur) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("nested json object/array in jsonl row is not supported", K(ret), K(line));
          break;
        } else if ('"' == *cur) {
          f.is_string_ = true;
          ++cur;
          const char *value_start = cur;
          while (cur < end && '"' != *cur) {
            if ('\\' == *cur && cur + 1 < end) {
              ++cur;
            }
            ++cur;
          }
          if (cur >= end) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          f.value_ = ObString(static_cast<int32_t>(cur - value_start), value_start);
          ++cur; // closing quote of the value
        } else {
          const char *value_start = cur;
          while (cur < end && ',' != *cur && '}' != *cur) {
            ++cur;
          }
          ObString raw = trim_ws(value_start, static_cast<int32_t>(cur - value_start));
          if (raw.length() == 4 && 0 == MEMCMP(raw.ptr(), "null", 4)) {
            f.is_null_ = true;
          } else {
            f.value_ = raw;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(fields.push_back(f))) {
          LOG_WARN("failed to push json field", K(ret));
        } else {
          cur = skip_ws(cur, end);
          if (cur < end && ',' == *cur) {
            cur = skip_ws(cur + 1, end);
          } else if (cur < end && '}' == *cur) {
            cur = skip_ws(cur + 1, end);
            break;
          } else if (cur >= end) {
            break;
          } else {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
        }
      }
    }
    if (OB_SUCC(ret) && cur < end) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("trailing content after json object", K(ret), K(line));
    }
  }
  return ret;
}

// Best-effort conversion of one raw string field into the declared column
// type. Conversion failure yields NULL, never an error.
int ObFileSqlScanner::convert_field(common::ObIAllocator &allocator, const ObString &raw,
                                    const ObFileSqlColumnDef &col_def, ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (raw.empty() ||
      (!def_.null_str_.empty() && 0 == raw.case_compare(def_.null_str_))) {
    obj.set_null();
  } else {
    switch (col_def.type_) {
      case ObIntType: {
        int err = 0;
        const int64_t value = ObCharset::strntoll(raw.ptr(), raw.length(), 10, &err);
        if (0 != err) {
          obj.set_null();
        } else {
          obj.set_int(value);
        }
        break;
      }
      case ObDoubleType: {
        char buf[64] = {0};
        MEMCPY(buf, raw.ptr(), MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
        char *end = NULL;
        const double value = ::strtod(buf, &end);
        if (NULL == end || end == buf || '\0' != *end) {
          obj.set_null();
        } else {
          obj.set_double(value);
        }
        break;
      }
      case ObTinyIntType: {
        if (0 == raw.case_compare("true") ||
            (1 == raw.length() && '1' == raw.ptr()[0])) {
          obj.set_tinyint(1);
        } else if (0 == raw.case_compare("false") ||
                   (1 == raw.length() && '0' == raw.ptr()[0])) {
          obj.set_tinyint(0);
        } else {
          obj.set_null();
        }
        break;
      }
      case ObDateType: {
        int32_t value = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(raw, value, ObDateSqlMode()))) {
          obj.set_null();
          ret = OB_SUCCESS;
        } else {
          obj.set_date(value);
        }
        break;
      }
      case ObDateTimeType: {
        int64_t value = 0;
        char buf[64] = {0};
        MEMCPY(buf, raw.ptr(), MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
        if (raw.length() > 10 && 'T' == buf[10]) {
          buf[10] = ' ';
        }
        ObString str(static_cast<int32_t>(::strlen(buf)), buf);
        ObTimeConvertCtx cvrt_ctx(NULL, false);
        if (OB_FAIL(ObTimeConverter::str_to_datetime(str, cvrt_ctx, value, NULL, 0))) {
          obj.set_null();
          ret = OB_SUCCESS;
        } else {
          obj.set_datetime(value);
        }
        break;
      }
      case ObVarcharType: {
        // deep copy with a trailing NUL so string datums stay safe
        char *ptr = static_cast<char*>(allocator.alloc(raw.length() + 1));
        if (OB_ISNULL(ptr)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(ptr, raw.ptr(), raw.length());
          ptr[raw.length()] = '\0';
          obj.set_varchar(ObString(raw.length(), ptr));
          obj.set_collation_type(CS_TYPE_INVALID == col_def.cs_type_
                                     ? CS_TYPE_UTF8MB4_BIN
                                     : col_def.cs_type_);
        }
        break;
      }
      default: {
        obj.set_null();
        break;
      }
    }
  }
  return ret;
}

int ObFileSqlScanner::csv_record_to_row(common::ObIAllocator &allocator,
                                        const common::ObSEArray<ObString, 32> &fields,
                                        ObObj *cells)
{
  int ret = OB_SUCCESS;
  const int64_t col_cnt = def_.columns_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
    const ObFileSqlColumnDef &col_def = def_.columns_.at(i);
    if (i >= fields.count()) {
      // fewer fields than columns: pad with NULL
      cells[i].set_null();
    } else if (OB_FAIL(convert_field(allocator, fields.at(i), col_def, cells[i]))) {
      LOG_WARN("failed to convert csv field", K(ret), K(i));
    }
  }
  // extra fields beyond the declared columns are ignored
  return ret;
}

int ObFileSqlScanner::json_record_to_row(common::ObIAllocator &allocator,
                                         const common::ObSEArray<ObFileSqlJsonField, 32> &fields,
                                         ObObj *cells)
{
  int ret = OB_SUCCESS;
  const int64_t col_cnt = def_.columns_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
    const ObFileSqlColumnDef &col_def = def_.columns_.at(i);
    const ObFileSqlJsonField *field = NULL;
    for (int64_t j = 0; OB_ISNULL(field) && j < fields.count(); ++j) {
      if (0 == col_def.name_.case_compare(fields.at(j).name_)) {
        field = &fields.at(j);
      }
    }
    if (OB_ISNULL(field) || field->is_null_) {
      // missing key or json null -> NULL
      cells[i].set_null();
    } else if (OB_FAIL(convert_field(allocator, field->value_, col_def, cells[i]))) {
      LOG_WARN("failed to convert json field", K(ret), K(i), K(col_def.name_));
    }
  }
  return ret;
}

int ObFileSqlScanner::get_next_batch(common::ObIAllocator &allocator, int64_t capacity,
                                     int64_t &count, ObNewRow *&rows)
{
  int ret = OB_SUCCESS;
  count = 0;
  rows = NULL;
  const int64_t col_cnt = def_.columns_.count();
  if (OB_UNLIKELY(capacity <= 0) || OB_UNLIKELY(col_cnt <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid file sql batch params", K(ret), K(capacity), K(col_cnt));
  } else if (OB_ISNULL(fp_) || OB_ISNULL(buf_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file sql scanner is not opened", K(ret));
  } else {
    ObNewRow *row_arr = static_cast<ObNewRow*>(allocator.alloc(sizeof(ObNewRow) * capacity));
    ObObj *cells = static_cast<ObObj*>(allocator.alloc(sizeof(ObObj) * capacity * col_cnt));
    if (OB_ISNULL(row_arr) || OB_ISNULL(cells)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      while (OB_SUCC(ret) && count < capacity) {
        bool has_record = false;
        if (ObFileSqlFormat::CSV == def_.format_) {
          if (OB_FAIL(read_csv_record(scratch_fields_))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break; // clean EOF
            }
          } else {
            has_record = true;
          }
        } else {
          ObString line;
          if (OB_FAIL(read_json_line(line))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break; // clean EOF
            }
          } else if (0 == trim_ws(line.ptr(), static_cast<int32_t>(line.length())).length()) {
            continue; // blank line
          } else if (OB_FAIL(parse_json_object(line, json_fields_))) {
          } else {
            has_record = true;
          }
        }
        if (OB_SUCC(ret) && has_record) {
          ObObj *row_cells = cells + count * col_cnt;
          if (ObFileSqlFormat::CSV == def_.format_) {
            if (OB_FAIL(csv_record_to_row(allocator, scratch_fields_, row_cells))) {
            }
          } else if (OB_FAIL(json_record_to_row(allocator, json_fields_, row_cells))) {
          }
          if (OB_SUCC(ret)) {
            new (row_arr + count) ObNewRow(row_cells, col_cnt);
            ++count;
          }
        }
      }
      if (OB_SUCC(ret) && count > 0) {
        rows = row_arr;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
