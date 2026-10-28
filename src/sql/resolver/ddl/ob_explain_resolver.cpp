/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/engine/table/ob_file_sql_def.h"
#include <cstdio>
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve_file_sql_describe(TableItem &file_table)
{
  int ret = OB_SUCCESS;
  ObFileSqlDef *src_def = static_cast<ObFileSqlDef *>(file_table.file_sql_def_);
  if (OB_ISNULL(src_def) || OB_ISNULL(params_.expr_factory_) || OB_ISNULL(params_.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid file sql describe context", K(ret));
  } else {
    // 1. materialize the describe rows into a temporary CSV
    char path_buf[common::MAX_PATH_SIZE] = {0};
    snprintf(path_buf, sizeof(path_buf), "/tmp/seekdb_describe_%ld_%ld.csv",
             static_cast<long>(getpid()), static_cast<long>(generate_table_id()));
    FILE *f = ::fopen(path_buf, "w");
    if (OB_ISNULL(f)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("fail to create describe temp file", K(ret));
    } else {
      ::fprintf(f, "column_name,column_type,null,key,default,extra\n");
      for (int64_t i = 0; OB_SUCC(ret) && i < src_def->columns_.count(); ++i) {
        const ObFileSqlColumnDef &col = src_def->columns_.at(i);
        const char *type_str = "VARCHAR";
        switch (col.type_) {
          case ObIntType: type_str = "BIGINT"; break;
          case ObDoubleType: type_str = "DOUBLE"; break;
          case ObTinyIntType: type_str = "BOOLEAN"; break;
          case ObDateType: type_str = "DATE"; break;
          case ObDateTimeType: type_str = "DATETIME"; break;
          default: type_str = "VARCHAR"; break;
        }
        ::fprintf(f, "%.*s,%s,%s,,,\n", col.name_.length(), col.name_.ptr(),
                  type_str, col.nullable_ ? "YES" : "NO");
      }
      ::fclose(f);

      // 2. scan the temp CSV through the normal file_sql machinery
      ObFileSqlDef *def = static_cast<ObFileSqlDef *>(
          params_.allocator_->alloc(sizeof(ObFileSqlDef)));
      ObSelectStmt *select_stmt = NULL;
      TableItem *table_item = NULL;
      if (OB_ISNULL(def)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        def = new (def) ObFileSqlDef(*params_.allocator_);
        def->format_ = ObFileSqlFormat::CSV;
        def->header_ = true;
        def->sample_size_ = -1;
        def->delim_ = ',';
        def->quote_ = '"';
        def->escape_ = '\0';
        if (OB_FAIL(ob_write_string(*params_.allocator_, ObString(path_buf), def->path_))) {
          LOG_ERROR("DESCDBG path write failed", K(ret));
        } else if (OB_FAIL(ObFileSqlInferUtils::infer_schema(*def))) {
          LOG_ERROR("DESCDBG infer failed", K(ret));
        }
        LOG_ERROR("DESCDBG after infer", K(ret), K(def->columns_.count()));
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(select_stmt = create_stmt<ObSelectStmt>())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("DESCDBG create select failed", K(ret));
      } else if (OB_ISNULL(table_item = select_stmt->create_table_item(*params_.allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("DESCDBG create table item failed", K(ret));
      } else {
        LOG_ERROR("DESCDBG table item created", K(ret));
        table_item->table_name_ = ObString("describe_file_sql");
        table_item->alias_name_ = table_item->table_name_;
        table_item->table_id_ = generate_table_id();
        table_item->type_ = TableItem::FILE_SQL;
        table_item->file_sql_def_ = static_cast<void *>(def);
        if (OB_FAIL(select_stmt->add_table_item(session_info_, table_item))) {
          LOG_ERROR("DESCDBG add table item failed", K(ret));
        } else {
          LOG_ERROR("DESCDBG table item added", K(ret));
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < def->columns_.count(); ++i) {
        const ObFileSqlColumnDef &col_def = def->columns_.at(i);
        ObColumnRefRawExpr *col_expr = NULL;
        ColumnItem column_item;
        sql::ObRawExprResType result_type;
        common::ObDataType data_type;
        data_type.set_obj_type(col_def.type_);
        data_type.set_collation_type(col_def.cs_type_);
        data_type.set_length(MAX(col_def.length_, 1));
        data_type.set_collation_level(common::CS_LEVEL_IMPLICIT);
        if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr))) {
        } else if (OB_ISNULL(col_expr)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_expr->set_ref_id(table_item->table_id_, static_cast<uint64_t>(i));
          result_type.set_meta(data_type.get_meta_type());
          result_type.set_accuracy(data_type.get_accuracy());
          col_expr->set_result_type(result_type);
          col_expr->set_column_attr(table_item->get_object_name(), col_def.name_);
          col_expr->unset_result_flag(NOT_NULL_FLAG);
          column_item.expr_ = col_expr;
          column_item.table_id_ = col_expr->get_table_id();
          column_item.column_id_ = col_expr->get_column_id();
          column_item.column_name_ = col_expr->get_column_name();
          if (OB_FAIL(col_expr->extract_info())) {
            LOG_ERROR("DESCDBG extract failed", K(ret), K(i));
          } else if (OB_FAIL(select_stmt->add_column_item(column_item))) {
            LOG_ERROR("DESCDBG add col item failed", K(ret), K(i));
          } else {
            SelectItem select_item;
            select_item.expr_ = col_expr;
            select_item.expr_name_ = col_def.name_;
            select_item.alias_name_ = col_def.name_;
            if (OB_FAIL(select_stmt->add_select_item(select_item))) {
              LOG_ERROR("DESCDBG add select item failed", K(ret), K(i));
            }
          }
        }
      }
      LOG_ERROR("DESCDBG before set_basic", K(ret), KP(select_stmt));
      if (OB_SUCC(ret)) {
        // the outer statement is a DESCRIBE; the result is an ordinary
        // select, so re-mark the session stmt type accordingly
        params_.session_info_->set_stmt_type(stmt::T_SELECT);
        set_basic_stmt(select_stmt);
      }
      LOG_ERROR("DESCDBG set_basic done", K(ret));
      // NOTE: the temp CSV must outlive the query execution (the file_sql
      // scanner opens it at run time); /tmp files are cleaned by the OS.
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (T_SELECT == child_node->type_) {
        // DuckDB-style DESCRIBE of a file_sql query: emit the
        // column_name/column_type/null/key/default/extra table instead of
        // the OB query plan.
        ObSelectStmt *child_select = static_cast<ObSelectStmt *>(child_stmt);
        TableItem *file_table = NULL;
        for (int64_t i = 0; OB_SUCC(ret) && OB_ISNULL(file_table)
                        && i < child_select->get_table_size(); ++i) {
          TableItem *t = child_select->get_table_item(i);
          if (OB_NOT_NULL(t) && t->is_file_sql()) {
            file_table = t;
          }
        }
        if (false && OB_NOT_NULL(file_table)) {
          ret = resolve_file_sql_describe(*file_table);
        } else {
          explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
          ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
          if (NULL != format) {
            switch (format->type_) {
            case T_OUTLINE:
            {
              explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
              opt.with_tree_line_ = true;
            } break;
            case T_EXTENDED:
            {
              explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
              opt.with_tree_line_ = true;
            } break;
            case T_PARTITIONS:
            {
              explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
              opt.with_tree_line_ = true;
            } break;
            case T_TRADITIONAL:
            {
              explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
              opt.with_tree_line_ = true;
            } break;
            case T_FORMAT_JSON:
            {
              explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
              opt.with_tree_line_ = false;
            } break;
            case T_BASIC: {
              explain_stmt->set_explain_format(EXPLAIN_BASIC);
              opt.with_tree_line_ = true;
              break;
            }
            case T_EXTENDED_NOADDR: {
              explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
              opt.with_tree_line_ = true;
              break;
            }
            case T_PLANREGRESS: {
              explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
              opt.with_tree_line_ = true;
              break;
            }
            default:
              // other formats keep the defaults
              break;
            }
          }
        }
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
      }
    }
  }
  return ret;
}

