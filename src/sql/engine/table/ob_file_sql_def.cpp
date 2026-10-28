/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_sql_def.h"
#include "lib/string/ob_sql_string.h"
#include "lib/timezone/ob_time_convert.h"
#include "lib/ob_date_unit_type.h"
#include "lib/charset/ob_charset.h"
#include "common/object/ob_object.h"

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
  } else if (OB_FAIL(ob_write_string(allocator_, other.null_str_, null_str_))) {
  } else if (OB_FAIL(columns_.assign(other.columns_))) {
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

static bool is_date_str(const ObString &s)
{
  if (s.length() != 10 || s.ptr()[4] != '-' || s.ptr()[7] != '-') {
    return false;
  }
  int y = 0, m = 0, d = 0;
  const char *p = s.ptr();
  if (!ObFastFormat::is_digit(p, 4) || !ObFastFormat::is_digit(p + 5, 2) || !ObFastFormat::is_digit(p + 8, 2)) {
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
    case ObDateType:
    case ObDateTimeType: {
      int64_t value = 0;
      ObDateSqlMode date_sql_mode;
      ObOTimestampData ts_value;
      const bool is_date = (type == ObDateType);
      const char *p = raw.ptr();
      char buf[32] = {0};
      MEMCPY(buf, p, MIN(raw.length(), static_cast<int64_t>(sizeof(buf) - 1)));
      if (p[10] == 'T') {
        buf[10] = ' ';
      }
      ObString str(strlen(buf), buf);
      if (OB_FAIL(ObTimeConverter::str_to_datetime_or_date(str, is_date, ts_value, date_sql_mode))) {
        // fall back to varchar on conversion failure
        obj.set_varchar(raw);
        obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        ret = OB_SUCCESS;
      } else {
        value = ts_value.time_us_;
        if (is_date) {
          obj.set_date(static_cast<int32_t>(value));
        } else {
          obj.set_datetime(value);
        }
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
    } else if (OB_FAIL(obj.to_datum(datum))) {
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

} // namespace sql
} // namespace oceanbase
