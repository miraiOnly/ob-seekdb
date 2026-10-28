/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_sql_def.h"
#include "ob_parquet_reader.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string_buffer.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/ob_date_unit_type.h"
#include "lib/charset/ob_charset.h"
#include "common/object/ob_object.h"
#include <cstdlib>
#include <cstring>
#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace oceanbase
{
namespace sql
{

int ObFileSqlDef::assign(const ObFileSqlDef &other)
{
  int ret = OB_SUCCESS;
  format_ = other.format_;
  header_ = other.header_;
  sample_size_ = other.sample_size_;
  delim_ = other.delim_;
  quote_ = other.quote_;
  escape_ = other.escape_;
  file_mtime_ = other.file_mtime_;
  file_size_ = other.file_size_;
  if (OB_FAIL(ob_write_string(allocator_, other.path_, path_))) {
  } else if (OB_FAIL(ob_write_string(allocator_, other.options_, options_))) {
  } else if (OB_FAIL(ob_write_string(allocator_, other.null_str_, null_str_))) {
  } else if (OB_FAIL(columns_.assign(other.columns_))) {
  }
  // ObSEArray::assign copies the column structs shallowly; re-point every
  // column name into this def's allocator so the copy is self-contained
  // (the phy spec deep copy must survive the statement arena).
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    if (OB_FAIL(ob_write_string(allocator_, other.columns_.at(i).name_, columns_.at(i).name_))) {
    }
  }
  return ret;
}

static bool is_bigint_str(const ObString &s)
{
  if (s.empty()) {
    return false;
  }
  const char *p = s.ptr();
  int64_t len = s.length();
  int64_t i = 0;
  if (p[0] == '+' || p[0] == '-') {
    i = 1;
  }
  if (i >= len) {
    return false;
  }
  for (; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  // reject leading zeros like 01 to stay deterministic with DuckDB
  if (len - i >= 2 && p[0] != '-' && p[0] != '+' && p[0] == '0' && len - i > 1) {
    return false;
  }
  // boundary check
  int err = 0;
  int64_t value = ObCharset::strntoll(p, len, 10, &err);
  return err == 0;
}

static bool is_double_str(const ObString &s)
{
  if (s.empty()) {
    return false;
  }
  // simple deterministic double check: [+-]?digits[.digits][eE[+-]digits]
  const char *p = s.ptr();
  int64_t len = s.length();
  int64_t i = 0;
  bool digits_before = false, digits_after = false, dot = false, exp = false;
  if (p[0] == '+' || p[0] == '-') {
    i = 1;
  }
  for (; i < len; i++) {
    char c = p[i];
    if (c >= '0' && c <= '9') {
      if (exp) { digits_after = true; } else { digits_before = true; }
    } else if (c == '.' && !dot && !exp) {
      dot = true;
    } else if ((c == 'e' || c == 'E') && !exp && (digits_before || dot)) {
      exp = true;
      if (i + 1 < len && (p[i + 1] == '+' || p[i + 1] == '-')) {
        i++;
      }
    } else {
      return false;
    }
  }
  return (digits_before || dot) && (!exp || digits_after);
}

static bool is_boolean_str(const ObString &s)
{
  return 0 == s.case_compare("true") || 0 == s.case_compare("false");
}

static bool is_digit_span(const char *p, const int64_t len)
{
  for (int64_t i = 0; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  return true;
}

static bool is_date_str(const ObString &s)
{
  if (s.length() != 10 || s.ptr()[4] != '-' || s.ptr()[7] != '-') {
    return false;
  }
  int y = 0, m = 0, d = 0;
  const char *p = s.ptr();
  if (!is_digit_span(p, 4) || !is_digit_span(p + 5, 2) || !is_digit_span(p + 8, 2)) {
    return false;
  }
  y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
  m = (p[5]-'0')*10 + (p[6]-'0');
  d = (p[8]-'0')*10 + (p[9]-'0');
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
    return false;
  }
  if (d > 28 && m == 2) {
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > 29 || !leap) {
      return false;
    }
  } else if (d == 31 && (m == 4 || m == 6 || m == 9 || m == 11)) {
    return false;
  }
  return true;
}

static bool is_datetime_str(const ObString &s)
{
  // YYYY-MM-DD HH:MM:SS[.ffffff] with optional T separator
  if (s.length() < 19) {
    return false;
  }
  const char *p = s.ptr();
  if (p[10] != ' ' && p[10] != 'T') {
    return false;
  }
  ObString date_part(10, p);
  if (!is_date_str(date_part)) {
    return false;
  }
  if (p[13] != ':' || p[16] != ':') {
    return false;
  }
  int h = (p[11]-'0')*10 + (p[12]-'0');
  int mi = (p[14]-'0')*10 + (p[15]-'0');
  int sec = (p[17]-'0')*10 + (p[18]-'0');
  if (h > 23 || mi > 59 || sec > 59) {
    return false;
  }
  int64_t len = s.length();
  if (len > 19) {
    if (p[19] != '.' || len > 26) {
      return false;
    }
    for (int64_t i = 20; i < len; i++) {
      if (p[i] < '0' || p[i] > '9') {
        return false;
      }
    }
  }
  return true;
}

int ObFileSqlInferUtils::infer_csv_type(common::ObIAllocator &allocator, const ObString &raw,
                                        ObObjType &type, int32_t &length)
{
  UNUSED(allocator);
  int ret = OB_SUCCESS;
  if (raw.empty()) {
    type = ObNullType;
    length = 0;
  } else if (is_bigint_str(raw)) {
    type = ObIntType;
    length = 20;
  } else if (is_double_str(raw)) {
    type = ObDoubleType;
    length = 22;
  } else if (is_boolean_str(raw)) {
    type = ObTinyIntType; // represented as 0/1
    length = 1;
  } else if (is_date_str(raw)) {
    type = ObDateType;
    length = 10;
  } else if (is_datetime_str(raw)) {
    type = ObDateTimeType;
    length = 19;
  } else {
    type = ObVarcharType;
    length = raw.length();
  }
  return ret;
}

int ObFileSqlInferUtils::infer_column_types(common::ObIAllocator &allocator,
                                            const common::ObIArray<ObString> &column_names,
                                            common::ObIArray<common::ObSEArray<ObString, 16> > &values,
                                            common::ObIArray<ObObjType> &types)
{
  int ret = OB_SUCCESS;
  const int64_t col_count = column_names.count();
  if (col_count != values.count() || types.count() != col_count) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < col_count; c++) {
      ObObjType merged = ObNullType;
      int32_t length = 0;
      for (int64_t r = 0; OB_SUCC(ret) && r < values.at(c).count(); r++) {
        const ObString &raw = values.at(c).at(r);
        ObObjType cur = ObNullType;
        int32_t cur_len = 0;
        if (OB_FAIL(infer_csv_type(allocator, raw, cur, cur_len))) {
        } else if (cur == ObNullType) {
          // NULL widens nothing
        } else if (merged == ObNullType) {
          merged = cur;
          length = cur_len;
        } else if (merged == cur) {
          length = MAX(length, cur_len);
        } else if ((merged == ObIntType && cur == ObDoubleType)
                   || (merged == ObDoubleType && cur == ObIntType)) {
          merged = ObDoubleType;
          length = 22;
        } else {
          merged = ObVarcharType;
          length = MAX(length, cur_len);
        }
      }
      if (OB_SUCC(ret)) {
        types.at(c) = merged == ObNullType ? ObVarcharType : merged;
      }
    }
  }
  return ret;
}

int ObFileSqlInferUtils::csv_to_obj(common::ObIAllocator &allocator, const ObString &raw,
                                    ObObjType type, ObObj &obj)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case ObIntType: {
      int err = 0;
      int64_t v = ObCharset::strntoll(raw.ptr(), raw.length(), 10, &err);
      obj.set_int(v);
      break;
    }
    case ObDoubleType: {
      double v = 0;
      char buf[64] = {0};
      MEMCPY(buf, raw.ptr(), MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
      v = strtod(buf, NULL);
      obj.set_double(v);
      break;
    }
    case ObTinyIntType: {
      obj.set_tinyint(raw.case_compare("true") == 0 ? 1 : 0);
      break;
    }
    case ObDateType: {
      int32_t value = 0;
      char buf[32] = {0};
      MEMCPY(buf, raw.ptr(), MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
      ObString str(strlen(buf), buf);
      if (OB_FAIL(ObTimeConverter::str_to_date(str, value, ObDateSqlMode()))) {
        // fall back to varchar on conversion failure
        obj.set_varchar(raw);
        obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        ret = OB_SUCCESS;
      } else {
        obj.set_date(value);
      }
      break;
    }
    case ObDateTimeType: {
      int64_t value = 0;
      char buf[32] = {0};
      MEMCPY(buf, raw.ptr(), MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
      if (raw.length() > 10 && 'T' == buf[10]) {
        buf[10] = ' ';
      }
      ObString str(strlen(buf), buf);
      ObTimeConvertCtx cvrt_ctx(NULL, false);
      if (OB_FAIL(ObTimeConverter::str_to_datetime(str, cvrt_ctx, value, NULL, 0))) {
        // fall back to varchar on conversion failure
        obj.set_varchar(raw);
        obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        ret = OB_SUCCESS;
      } else {
        obj.set_datetime(value);
      }
      break;
    }
    default: {
      obj.set_varchar(raw);
      obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      break;
    }
  }
  UNUSED(allocator);
  return ret;
}

int ObFileSqlInferUtils::csv_to_datums(common::ObIAllocator &allocator, const ObString &raw,
                                       ObObjType type, ObDatum &datum, ObObjMeta &meta)
{
  int ret = OB_SUCCESS;
  meta = ObObjMeta();
  meta.set_type(type);
  if (raw.empty() || (type == ObVarcharType && raw.length() == 0)) {
    // NULL representation handled by the caller (null_str match); empty
    // string stays an empty varchar.
  }
  if (type == ObNullType) {
    datum.set_null();
  } else {
    ObObj obj;
    if (OB_FAIL(csv_to_obj(allocator, raw, type, obj))) {
    } else if (obj.is_null()) {
      datum.set_null();
    } else {
      switch (type) {
        case ObIntType: {
          datum.set_int(obj.get_int());
          break;
        }
        case ObDoubleType: {
          datum.set_double(obj.get_double());
          break;
        }
        case ObTinyIntType: {
          datum.set_int(static_cast<int64_t>(obj.get_tinyint()));
          break;
        }
        case ObDateType: {
          datum.set_date(obj.get_date());
          break;
        }
        case ObDateTimeType: {
          datum.set_datetime(obj.get_datetime());
          break;
        }
        case ObVarcharType: {
          datum.set_string(obj.get_string());
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
      }
    }
  }
  return ret;
}

bool ObFileSqlInferUtils::check_file_changed(int64_t mtime, int64_t size, const ObString &path,
                                             int64_t &new_mtime, int64_t &new_size)
{
  struct stat st;
  new_mtime = 0;
  new_size = 0;
  if (0 != ::stat(path.ptr(), &st)) {
    return true; // missing file counts as changed
  }
  new_mtime = st.st_mtime;
  new_size = st.st_size;
  return mtime != 0 && (mtime != new_mtime || size != new_size);
}

namespace {

const int64_t GLOB_FILE_COLUMN_LENGTH = 1024;

int read_whole_file(ObFileSqlDef &def, char *&buf, int64_t &len)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  struct stat st;
  buf = NULL;
  len = 0;
  MEMSET(&st, 0, sizeof(st));
  if (OB_UNLIKELY(def.path_.empty() || OB_ISNULL(def.path_.ptr()))) {
    ret = OB_FILE_NOT_EXIST;
  } else if (def.path_.length() + 1 > static_cast<int64_t>(common::MAX_PATH_SIZE)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    char path_buf[common::MAX_PATH_SIZE] = {0};
    MEMCPY(path_buf, def.path_.ptr(), def.path_.length());
    path_buf[def.path_.length()] = '\0';
    if ((fd = ::open(path_buf, O_RDONLY)) < 0) {
      ret = OB_FILE_NOT_EXIST;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (0 != ::fstat(fd, &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else if (OB_UNLIKELY(st.st_size < 0)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    def.file_mtime_ = st.st_mtime;
    def.file_size_ = st.st_size;
    if (st.st_size > 0) {
      void *mem = def.allocator_.alloc(st.st_size + 1);
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        buf = static_cast<char *>(mem);
        int64_t off = 0;
        while (OB_SUCC(ret) && off < st.st_size) {
          ssize_t r = ::read(fd, buf + off, st.st_size - off);
          if (r > 0) {
            off += r;
          } else if (0 == r) {
            ret = OB_IO_ERROR;
          } else if (EINTR == errno) {
            // interrupted, retry
          } else {
            ret = OB_IO_ERROR;
          }
        }
        if (OB_SUCC(ret)) {
          len = off;
          buf[len] = '\0';
        }
      }
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }
  return ret;
}

// Parse one csv row starting at data[pos]; on return pos points right after
// the terminating newline (or at len when the last row has no trailing
// newline). Field values are written into the allocator. Supports quoted
// fields, doubled-quote escapes, the escape char and CRLF line endings.
int parse_csv_row(common::ObIAllocator &allocator, const char *data, int64_t len,
                  int64_t &pos, char delim, char quote, char escape,
                  common::ObSEArray<ObString, 16> &fields)
{
  int ret = OB_SUCCESS;
  ObStringBuffer field_buf(&allocator);
  bool in_quotes = false;
  bool quoted_field = false;
  bool field_started = false;
  fields.reset();
  while (OB_SUCC(ret) && pos < len) {
    const char c = data[pos];
    if (in_quotes) {
      if (c == quote) {
        if (pos + 1 < len && data[pos + 1] == quote) {
          // doubled quote: literal quote char
          if (OB_FAIL(field_buf.append(&c, 1))) {
          } else {
            pos += 2;
          }
        } else {
          in_quotes = false;
          pos += 1;
        }
      } else if ('\0' != escape && c == escape && pos + 1 < len) {
        if (OB_FAIL(field_buf.append(data + pos + 1, 1))) {
        } else {
          pos += 2;
        }
      } else if (OB_FAIL(field_buf.append(data + pos, 1))) {
      } else {
        pos += 1;
      }
    } else if (quoted_field) {
      // closing quote already consumed; skip stray chars until delim/newline
      if ('\n' == c || c == delim) {
        quoted_field = false;
      } else {
        pos += 1;
      }
    } else if (c == delim) {
      ObString out;
      if (OB_FAIL(ob_write_string(allocator, field_buf.string(), out))) {
      } else if (OB_FAIL(fields.push_back(out))) {
      } else {
        field_buf.reuse();
        field_started = false;
        pos += 1;
      }
    } else if ('\n' == c) {
      pos += 1;
      break;
    } else if ('\r' == c && pos + 1 < len && '\n' == data[pos + 1]) {
      pos += 2;
      break;
    } else if (c == quote && !field_started) {
      in_quotes = true;
      quoted_field = true;
      field_started = true;
      pos += 1;
    } else if ('\0' != escape && c == escape && pos + 1 < len) {
      if (OB_FAIL(field_buf.append(data + pos + 1, 1))) {
      } else {
        field_started = true;
        pos += 2;
      }
    } else if (OB_FAIL(field_buf.append(data + pos, 1))) {
    } else {
      field_started = true;
      pos += 1;
    }
  }
  if (OB_SUCC(ret)) {
    ObString out;
    if (OB_FAIL(ob_write_string(allocator, field_buf.string(), out))) {
    } else if (OB_FAIL(fields.push_back(out))) {
    }
  }
  return ret;
}

// Merge the candidate type of every value into the final column type.
// NULL widens nothing; int + double -> double; any other conflict -> varchar;
// all NULL -> varchar(1).
int merge_column_type(common::ObIAllocator &allocator,
                      const common::ObIArray<ObString> &vals,
                      ObFileSqlColumnDef &col)
{
  int ret = OB_SUCCESS;
  ObObjType merged = ObNullType;
  int32_t length = 0;
  for (int64_t r = 0; OB_SUCC(ret) && r < vals.count(); ++r) {
    ObObjType cur = ObNullType;
    int32_t cur_len = 0;
    if (OB_FAIL(ObFileSqlInferUtils::infer_csv_type(allocator, vals.at(r), cur, cur_len))) {
    } else if (ObNullType == cur) {
      // NULL widens nothing
    } else if (ObNullType == merged) {
      merged = cur;
      length = cur_len;
    } else if (merged == cur) {
      length = MAX(length, cur_len);
    } else if ((ObIntType == merged && ObDoubleType == cur)
               || (ObDoubleType == merged && ObIntType == cur)) {
      merged = ObDoubleType;
      length = MAX(length, cur_len);
    } else {
      merged = ObVarcharType;
      length = MAX(length, cur_len);
    }
  }
  if (OB_SUCC(ret)) {
    col.type_ = ObNullType == merged ? ObVarcharType : merged;
    col.length_ = MAX(length, 1);
    col.cs_type_ = CS_TYPE_UTF8MB4_BIN;
    col.precision_ = -1;
    col.scale_ = -1;
  }
  return ret;
}

int infer_csv_schema(ObFileSqlDef &def, const char *data, int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t col_count = 0;
  const char escape = '\0' == def.escape_ ? '"' : def.escape_;
  common::ObSEArray<common::ObSEArray<ObString, 16>, 8> values;
  while (OB_SUCC(ret) && pos < len) {
    common::ObSEArray<ObString, 16> fields;
    if (OB_FAIL(parse_csv_row(def.allocator_, data, len, pos, def.delim_, def.quote_,
                              escape, fields))) {
      LOG_WARN("fail to parse csv row", K(ret), K(pos));
    } else if (1 == fields.count() && fields.at(0).empty()) {
      // blank line, skip
    } else if (0 == col_count) {
      // first row: header names or generated names; fixes the column count
      col_count = fields.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        ObFileSqlColumnDef col;
        col.type_ = ObNullType;
        col.cs_type_ = CS_TYPE_UTF8MB4_BIN;
        col.length_ = 0;
        col.precision_ = -1;
        col.scale_ = -1;
        if (def.header_) {
          col.name_ = fields.at(i);
        } else {
          char name_buf[32] = {0};
          snprintf(name_buf, sizeof(name_buf), "column%ld", i);
          if (OB_FAIL(ob_write_string(def.allocator_, ObString(name_buf), col.name_))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(def.columns_.push_back(col))) {
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        if (OB_FAIL(values.push_back(common::ObSEArray<ObString, 16>()))) {
        }
      }
      if (OB_SUCC(ret) && !def.header_) {
        // no header: the first row is real data
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
          ObString v = fields.at(i);
          if (!def.null_str_.empty() && v == def.null_str_) {
            v.reset();
          }
          if (OB_FAIL(values[i].push_back(v))) {
          }
        }
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        ObString v;
        if (i < fields.count()) {
          v = fields.at(i);
          if (!def.null_str_.empty() && v == def.null_str_) {
            v.reset();
          }
        }
        if (OB_FAIL(values[i].push_back(v))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && 0 == col_count) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("empty file sql csv file", K(ret), K(def.path_));
  }
  // fix empty column names
  for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
    ObFileSqlColumnDef &col = def.columns_[i];
    if (col.name_.empty()) {
      char name_buf[32] = {0};
      snprintf(name_buf, sizeof(name_buf), "column%ld", i);
      if (OB_FAIL(ob_write_string(def.allocator_, ObString(name_buf), col.name_))) {
      }
    }
  }
  // duckdb style duplicate names: second occurrence gets __2, third __3 ...
  for (int64_t i = 1; OB_SUCC(ret) && i < col_count; ++i) {
    ObFileSqlColumnDef &col = def.columns_[i];
    int64_t dup_cnt = 0;
    for (int64_t j = 0; j < i; ++j) {
      if (ObCharset::case_insensitive_equal(def.columns_.at(j).name_, col.name_)) {
        ++dup_cnt;
      }
    }
    if (dup_cnt > 0) {
      char suffix[32] = {0};
      snprintf(suffix, sizeof(suffix), "__%ld", dup_cnt + 1);
      ObSqlString new_name;
      ObString out;
      if (OB_FAIL(new_name.append(col.name_))) {
      } else if (OB_FAIL(new_name.append(suffix))) {
      } else if (OB_FAIL(ob_write_string(def.allocator_, new_name.string(), out))) {
      } else {
        col.name_ = out;
      }
    }
  }
  // merge per-column types
  for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
    if (OB_FAIL(merge_column_type(def.allocator_, values[i], def.columns_.at(i)))) {
    }
  }
  return ret;
}

int infer_json_schema(ObFileSqlDef &def, const char *data, int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t row_count = 0;
  common::ObSEArray<ObString, 8> names;
  common::ObSEArray<common::ObSEArray<ObString, 16>, 8> values;
  while (OB_SUCC(ret) && pos < len) {
    const char *line_start = data + pos;
    const char *nl = static_cast<const char *>(memchr(line_start, '\n', len - pos));
    const int64_t line_len = OB_NOT_NULL(nl) ? nl - line_start : len - pos;
    pos += line_len + (OB_NOT_NULL(nl) ? 1 : 0);
    // trim spaces/tabs/CR around the line
    int64_t start = 0;
    int64_t end = line_len;
    while (start < end && (' ' == line_start[start] || '\t' == line_start[start]
                           || '\r' == line_start[start])) {
      ++start;
    }
    while (end > start && (' ' == line_start[end - 1] || '\t' == line_start[end - 1]
                           || '\r' == line_start[end - 1])) {
      --end;
    }
    ObString line(end - start, line_start + start);
    if (line.empty()) {
      continue;  // skip blank lines
    }
    ObJsonNode *tree = NULL;
    if (OB_FAIL(ObJsonParser::get_tree(&def.allocator_, line, tree))) {
      LOG_WARN("fail to parse json line", K(ret), K(line));
    } else if (OB_ISNULL(tree) || ObJsonNodeType::J_OBJECT != tree->json_type()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("file sql json line must be a json object", K(ret), K(line));
    } else {
      ObJsonObject *obj = static_cast<ObJsonObject *>(tree);
      // NULL placeholder on every known column for this row
      for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
        if (OB_FAIL(values[c].push_back(ObString()))) {
        }
      }
      for (uint64_t i = 0; OB_SUCC(ret) && i < obj->element_count(); ++i) {
        ObString key;
        ObJsonNode *val = NULL;
        if (OB_FAIL(obj->get_value_by_idx(i, key, val))) {
        } else if (OB_ISNULL(val)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          int64_t col_idx = -1;
          for (int64_t c = 0; -1 == col_idx && c < names.count(); ++c) {
            if (0 == names[c].case_compare(key)) {
              col_idx = c;
            }
          }
          if (-1 == col_idx) {
            // union schema: append a new column, backfill NULLs for earlier rows
            col_idx = names.count();
            ObString saved_key;
            if (OB_FAIL(ob_write_string(def.allocator_, key, saved_key))) {
            } else if (OB_FAIL(names.push_back(saved_key))) {
            } else if (OB_FAIL(values.push_back(common::ObSEArray<ObString, 16>()))) {
            } else {
              for (int64_t r = 0; OB_SUCC(ret) && r < row_count; ++r) {
                if (OB_FAIL(values[col_idx].push_back(ObString()))) {
                }
              }
              // reserve the current row's slot as well
              if (OB_SUCC(ret) && OB_FAIL(values[col_idx].push_back(ObString()))) {
              }
            }
          }
          ObString raw;
          if (OB_FAIL(ret)) {
          } else if (ObJsonNodeType::J_STRING == val->json_type()) {
            raw = static_cast<ObJsonString *>(val)->value();
          } else if (ObJsonNodeType::J_INT == val->json_type()) {
            char buf[32] = {0};
            snprintf(buf, sizeof(buf), "%ld", val->get_int());
            if (OB_FAIL(ob_write_string(def.allocator_, ObString(buf), raw))) {
            }
          } else if (ObJsonNodeType::J_UINT == val->json_type()) {
            char buf[32] = {0};
            snprintf(buf, sizeof(buf), "%lu", val->get_uint());
            if (OB_FAIL(ob_write_string(def.allocator_, ObString(buf), raw))) {
            }
          } else if (ObJsonNodeType::J_DOUBLE == val->json_type()) {
            // format the double value; keep double-ness even for integral
            // doubles like 1e2 so the column stays a double
            char buf[64] = {0};
            snprintf(buf, sizeof(buf), "%.17g", val->get_double());
            if (OB_ISNULL(strchr(buf, '.')) && OB_ISNULL(strchr(buf, 'e'))
                && OB_ISNULL(strchr(buf, 'E')) && OB_ISNULL(strchr(buf, 'n'))) {
              strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
            }
            if (OB_FAIL(ob_write_string(def.allocator_, ObString(buf), raw))) {
            }
          } else if (ObJsonNodeType::J_BOOLEAN == val->json_type()) {
            raw = ObString(val->get_boolean() ? "true" : "false");
          } else if (ObJsonNodeType::J_NULL == val->json_type()) {
            // null: empty string, widens nothing
          } else if (ObJsonNodeType::J_OBJECT == val->json_type()
                     || ObJsonNodeType::J_ARRAY == val->json_type()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("nested json value is not supported in file sql", K(ret), K(key));
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unsupported json value type", K(ret), K(val->json_type()));
          }
          if (OB_SUCC(ret)) {
            values[col_idx][values[col_idx].count() - 1] = raw;
          }
        }
      }
      if (OB_SUCC(ret)) {
        ++row_count;
      }
    }
  }
  if (OB_SUCC(ret) && 0 == names.count()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("empty file sql json file", K(ret), K(def.path_));
  }
  for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
    ObFileSqlColumnDef col;
    col.name_ = names[c];
    if (OB_FAIL(merge_column_type(def.allocator_, values[c], col))) {
    } else if (OB_FAIL(def.columns_.push_back(col))) {
    }
  }
  return ret;
}

} // anonymous namespace

int ObFileSqlInferUtils::infer_schema(ObFileSqlDef &def)
{
  int ret = OB_SUCCESS;
  char *data = NULL;
  int64_t len = 0;
  def.columns_.reset();
  switch (def.format_) {
    case ObFileSqlFormat::GLOB: {
      ObFileSqlColumnDef col;
      col.name_ = ObString("file");
      col.type_ = ObVarcharType;
      col.cs_type_ = CS_TYPE_UTF8MB4_BIN;
      col.length_ = GLOB_FILE_COLUMN_LENGTH;
      col.precision_ = -1;
      col.scale_ = -1;
      if (OB_FAIL(def.columns_.push_back(col))) {
      }
      break;
    }
    case ObFileSqlFormat::CSV: {
      if (OB_FAIL(read_whole_file(def, data, len))) {
        LOG_WARN("fail to read file sql csv file", K(ret), K(def.path_));
      } else if (OB_FAIL(infer_csv_schema(def, data, len))) {
        LOG_WARN("fail to infer file sql csv schema", K(ret), K(def.path_));
      }
      break;
    }
    case ObFileSqlFormat::JSON: {
      if (OB_FAIL(read_whole_file(def, data, len))) {
        LOG_WARN("fail to read file sql json file", K(ret), K(def.path_));
      } else if (OB_FAIL(infer_json_schema(def, data, len))) {
        LOG_WARN("fail to infer file sql json schema", K(ret), K(def.path_));
      }
      break;
    }
    case ObFileSqlFormat::PARQUET: {
      ObParquetFileInfo pq_info;
      if (OB_FAIL(read_whole_file(def, data, len))) {
        LOG_WARN("fail to read parquet file", K(ret), K(def.path_));
      } else if (OB_FAIL(ob_parse_parquet_footer(data, len, pq_info))) {
        LOG_WARN("fail to parse parquet footer", K(ret), K(def.path_));
      } else {
        ObFileSqlColumnDef col;
        col.name_ = pq_info.col_name_;
        col.cs_type_ = CS_TYPE_UTF8MB4_BIN;
        col.precision_ = -1;
        col.scale_ = -1;
        if (1 == pq_info.physical_type_ || 2 == pq_info.physical_type_) { // INT32/INT64
          col.type_ = ObIntType;
          col.length_ = 20;
        } else if (5 == pq_info.physical_type_) { // DOUBLE
          col.type_ = ObDoubleType;
          col.length_ = 22;
        } else {
          col.type_ = ObVarcharType;
          col.length_ = 1024;
        }
        if (OB_FAIL(def.columns_.push_back(col))) {
          LOG_WARN("fail to push parquet column", K(ret));
        }
      }
      break;
    }
    default: {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("file sql format is not supported for schema inference", K(ret), K(def.format_));
      break;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
