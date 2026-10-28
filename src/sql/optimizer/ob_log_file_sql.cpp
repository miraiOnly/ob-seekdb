/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_file_sql.h"
#include "sql/resolver/dml/ob_dml_stmt.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

// Collects the explicitly referenced columns of this table; they are the
// output produced by the file scan operator.
int ObLogFileSql::generate_access_exprs()
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      const ColumnItem *col_item = stmt->get_column_item(i);
      if (OB_ISNULL(col_item) || OB_ISNULL(col_item->expr_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (col_item->table_id_ == table_id_ &&
                 col_item->expr_->is_explicited_reference() &&
                 OB_FAIL(access_exprs_.push_back(col_item->expr_))) {
      } else { /* do nothing */ }
    }
  }
  return ret;
}

int ObLogFileSql::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < access_exprs_.count(); ++i) {
    ObRawExpr *value_col = access_exprs_.at(i);
    if (OB_FAIL(mark_expr_produced(value_col, branch_id_, id_, ctx))) {
    } else if (!is_plan_root() && OB_FAIL(add_var_to_array_no_dup(output_exprs_, value_col))) {
    } else { /* do nothing */ }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
    }
  }
  return ret;
}

int ObLogFileSql::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  // no document/value expressions in file sql: only the produced columns
  if (OB_FAIL(generate_access_exprs())) {
  } else if (OB_FAIL(append(all_exprs, access_exprs_))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
  }
  return ret;
}

int ObLogFileSql::get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
  } else {
    const ObString &name = get_table_name();
    BUF_PRINT_OB_STR(name.ptr(), name.length(),
                     plan_item.object_alias_, plan_item.object_alias_len_);
    BUF_PRINT_STR("FILE_SQL", plan_item.object_type_, plan_item.object_type_len_);
    if (OB_SUCC(ret) && OB_NOT_NULL(file_sql_def_)) {
      const ObString &path = file_sql_def_->path_;
      BUF_PRINT_OB_STR(path.ptr(), path.length(),
                       plan_item.special_predicates_, plan_item.special_predicates_len_);
    }
  }
  return ret;
}

uint64_t ObLogFileSql::hash(uint64_t seed) const
{
  seed = do_hash(table_name_, seed);
  seed = ObLogicalOperator::hash(seed);
  return seed;
}

int ObLogFileSql::est_cost()
{
  int ret = OB_SUCCESS;
  // file scan is a local leaf operator; use a constant estimate.
  set_card(1.0);
  set_op_cost(1.0);
  set_cost(1.0);
  return ret;
}

} // namespace sql
} // namespace oceanbase
