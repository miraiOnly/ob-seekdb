/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#pragma once

#include "sql/optimizer/ob_logical_operator.h"
#include "sql/engine/table/ob_file_sql_def.h"

namespace oceanbase
{
namespace sql
{

// Leaf logical operator for the "file as database" table functions
// (read_csv/read_json/read_parquet/glob). Mirrors ObLogJsonTable: it carries
// the table id, the table name and the file sql definition (borrowed from
// TableItem::file_sql_def_, owned by the resolver/statement arena). There is
// no document/value expression: the columns of this table are all produced by
// the file scan operator itself.
class ObLogFileSql : public ObLogicalOperator
{
public:
  ObLogFileSql(ObLogPlan &plan)
      : ObLogicalOperator(plan),
        table_id_(OB_INVALID_ID),
        table_name_(),
        file_sql_def_(NULL),
        access_exprs_() {}

  virtual ~ObLogFileSql() {}
  virtual uint64_t hash(uint64_t seed) const override;
  int generate_access_exprs();
  ObIArray<ObRawExpr*> &get_access_exprs() { return access_exprs_; }
  virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  void set_table_id(uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }

  inline common::ObString &get_table_name() { return table_name_; }
  inline const common::ObString &get_table_name() const { return table_name_; }
  inline void set_table_name(const common::ObString &table_name) { table_name_ = table_name; }

  // NOTE(compile-dependency): points at TableItem::file_sql_def_, which is
  // added to ObDMLStmt by the resolver patch (main agent).
  void set_file_sql_def(ObFileSqlDef *file_sql_def) { file_sql_def_ = file_sql_def; }
  ObFileSqlDef *get_file_sql_def() const { return file_sql_def_; }

  virtual int get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item) override;
  // file scan is a local leaf: constant estimate
  virtual int est_cost() override;
private:
  uint64_t table_id_;
  common::ObString table_name_;
  ObFileSqlDef *file_sql_def_;
  common::ObSEArray<ObRawExpr*, 4, common::ModulePageAllocator, true> access_exprs_;

  DISALLOW_COPY_AND_ASSIGN(ObLogFileSql);
};

} // namespace sql
} // namespace oceanbase
