#include "lakehouse/paimon_table_function.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "lakehouse/paimon_arrow_stream.h"
#include "paimon/defs.h"
#include "paimon/predicate/literal.h"
#include "paimon/predicate/predicate.h"
#include "paimon/predicate/predicate_builder.h"
#include "paimon/scan_context.h"
#include "paimon/schema/schema.h"
#include "paimon/table/source/plan.h"
#include "paimon/table/source/split.h"
#include "paimon/table/source/table_scan.h"

namespace myduck::lakehouse {
namespace {

struct PaimonScanFactory : public duckdb::ArrowScanFactory {
  std::string table_path;
  std::vector<std::string> field_names;
  std::vector<std::shared_ptr<paimon::Split>> splits;

  void GetSchema(ArrowSchema &schema) override {
    auto schema_res = LoadPaimonSchemaFromPath(table_path);
    if (!schema_res.ok()) {
      throw duckdb::IOException("paimon_scan: failed to load schema for '%s': %s",
                                table_path, schema_res.status().ToString());
    }
    auto arrow_res = schema_res.value()->GetArrowSchema();
    if (!arrow_res.ok()) {
      throw duckdb::IOException("paimon_scan: failed to export Arrow schema for '%s': %s",
                                table_path, arrow_res.status().ToString());
    }
    schema = *arrow_res.value();
    arrow_res.value()->release = nullptr;
  }

  duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper> ProduceStream(
      duckdb::ArrowStreamParameters &parameters) override {
    std::vector<std::string> projected = parameters.projected_columns.columns;
    if (projected.empty() && !field_names.empty()) {
      projected.push_back(field_names[0]);
    }
    auto std_stream = CreatePaimonArrowStream(table_path, splits, projected);
    return duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper>(std_stream.release());
  }
};

struct PaimonScanFunctionData : public duckdb::ArrowScanFunctionData {
  explicit PaimonScanFunctionData(duckdb::shared_ptr<PaimonScanFactory> paimon_factory_p)
      : duckdb::ArrowScanFunctionData(paimon_factory_p),
        paimon_factory(std::move(paimon_factory_p)) {}

  std::string table_path;
  int64_t snapshot_id = -1;
  std::vector<std::string> field_names;
  std::vector<paimon::FieldType> field_types;
  std::shared_ptr<paimon::Predicate> scan_predicate;
  duckdb::shared_ptr<PaimonScanFactory> paimon_factory;
};

std::optional<paimon::Literal> ConvertDuckValueToPaimonLiteral(
    const duckdb::Value &val, paimon::FieldType field_type) {
  if (val.IsNull()) {
    return std::nullopt;
  }
  try {
    switch (field_type) {
      case paimon::FieldType::BOOLEAN: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::BOOLEAN);
        return paimon::Literal(v.GetValue<bool>());
      }
      case paimon::FieldType::TINYINT: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::TINYINT);
        return paimon::Literal(v.GetValue<int8_t>());
      }
      case paimon::FieldType::SMALLINT: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::SMALLINT);
        return paimon::Literal(v.GetValue<int16_t>());
      }
      case paimon::FieldType::INT: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::INTEGER);
        return paimon::Literal(v.GetValue<int32_t>());
      }
      case paimon::FieldType::BIGINT: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::BIGINT);
        return paimon::Literal(v.GetValue<int64_t>());
      }
      case paimon::FieldType::FLOAT: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::FLOAT);
        return paimon::Literal(v.GetValue<float>());
      }
      case paimon::FieldType::DOUBLE: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::DOUBLE);
        return paimon::Literal(v.GetValue<double>());
      }
      case paimon::FieldType::STRING: {
        std::string s = val.ToString();
        return paimon::Literal(paimon::FieldType::STRING, s.data(), s.size());
      }
      case paimon::FieldType::DATE: {
        auto v = val.DefaultCastAs(duckdb::LogicalType::DATE);
        return paimon::Literal(paimon::FieldType::DATE, v.GetValue<duckdb::date_t>().days);
      }
      default:
        return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

duckdb::ExpressionType FlipComparison(duckdb::ExpressionType type) {
  switch (type) {
    case duckdb::ExpressionType::COMPARE_LESSTHAN:
      return duckdb::ExpressionType::COMPARE_GREATERTHAN;
    case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
      return duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO;
    case duckdb::ExpressionType::COMPARE_GREATERTHAN:
      return duckdb::ExpressionType::COMPARE_LESSTHAN;
    case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
      return duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO;
    default:
      return type;
  }
}

std::shared_ptr<paimon::Predicate> TryBuildPaimonPredicate(
    const duckdb::LogicalGet &get, const PaimonScanFunctionData &bind_data,
    const duckdb::Expression &expr) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_FUNCTION &&
      duckdb::BoundComparisonExpression::IsComparison(expr)) {
    const auto &func_expr = expr.Cast<duckdb::BoundFunctionExpression>();
    const auto &left = duckdb::BoundComparisonExpression::Left(func_expr);
    const auto &right = duckdb::BoundComparisonExpression::Right(func_expr);

    const duckdb::BoundColumnRefExpression *col_ref = nullptr;
    const duckdb::BoundConstantExpression *const_expr = nullptr;
    duckdb::ExpressionType cmp_type = expr.GetExpressionType();

    if (left.GetExpressionClass() == duckdb::ExpressionClass::BOUND_COLUMN_REF &&
        right.GetExpressionClass() == duckdb::ExpressionClass::BOUND_CONSTANT) {
      col_ref = &left.Cast<duckdb::BoundColumnRefExpression>();
      const_expr = &right.Cast<duckdb::BoundConstantExpression>();
    } else if (left.GetExpressionClass() == duckdb::ExpressionClass::BOUND_CONSTANT &&
               right.GetExpressionClass() == duckdb::ExpressionClass::BOUND_COLUMN_REF) {
      const_expr = &left.Cast<duckdb::BoundConstantExpression>();
      col_ref = &right.Cast<duckdb::BoundColumnRefExpression>();
      cmp_type = FlipComparison(cmp_type);
    }

    if (!col_ref || !const_expr || col_ref->Binding().table_index != get.table_index) {
      return nullptr;
    }

    const auto &col_index = get.GetColumnIndex(col_ref->Binding());
    if (col_index.IsVirtualColumn()) {
      return nullptr;
    }
    auto field_idx = col_index.GetPrimaryIndex();
    if (field_idx >= bind_data.field_names.size() || field_idx >= bind_data.field_types.size()) {
      return nullptr;
    }

    const std::string &field_name = bind_data.field_names[field_idx];
    paimon::FieldType field_type = bind_data.field_types[field_idx];
    auto literal_opt = ConvertDuckValueToPaimonLiteral(const_expr->GetValue(), field_type);
    if (!literal_opt.has_value()) {
      return nullptr;
    }

    int32_t idx32 = static_cast<int32_t>(field_idx);
    switch (cmp_type) {
      case duckdb::ExpressionType::COMPARE_EQUAL:
        return paimon::PredicateBuilder::Equal(idx32, field_name, field_type, *literal_opt);
      case duckdb::ExpressionType::COMPARE_NOTEQUAL:
        return paimon::PredicateBuilder::NotEqual(idx32, field_name, field_type, *literal_opt);
      case duckdb::ExpressionType::COMPARE_LESSTHAN:
        return paimon::PredicateBuilder::LessThan(idx32, field_name, field_type, *literal_opt);
      case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return paimon::PredicateBuilder::LessOrEqual(idx32, field_name, field_type, *literal_opt);
      case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        return paimon::PredicateBuilder::GreaterThan(idx32, field_name, field_type, *literal_opt);
      case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return paimon::PredicateBuilder::GreaterOrEqual(idx32, field_name, field_type,
                                                        *literal_opt);
      default:
        return nullptr;
    }
  }

  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_OPERATOR) {
    const auto &op_expr = expr.Cast<duckdb::BoundOperatorExpression>();
    if ((expr.GetExpressionType() == duckdb::ExpressionType::OPERATOR_IS_NULL ||
         expr.GetExpressionType() == duckdb::ExpressionType::OPERATOR_IS_NOT_NULL) &&
        op_expr.GetChildren().size() == 1 &&
        op_expr.GetChildren()[0]->GetExpressionClass() ==
            duckdb::ExpressionClass::BOUND_COLUMN_REF) {
      const auto &col_ref = op_expr.GetChildren()[0]->Cast<duckdb::BoundColumnRefExpression>();
      if (col_ref.Binding().table_index != get.table_index) {
        return nullptr;
      }
      const auto &col_index = get.GetColumnIndex(col_ref.Binding());
      if (col_index.IsVirtualColumn()) {
        return nullptr;
      }
      auto field_idx = col_index.GetPrimaryIndex();
      if (field_idx >= bind_data.field_names.size() || field_idx >= bind_data.field_types.size()) {
        return nullptr;
      }
      int32_t idx32 = static_cast<int32_t>(field_idx);
      if (expr.GetExpressionType() == duckdb::ExpressionType::OPERATOR_IS_NULL) {
        return paimon::PredicateBuilder::IsNull(idx32, bind_data.field_names[field_idx],
                                                bind_data.field_types[field_idx]);
      }
      return paimon::PredicateBuilder::IsNotNull(idx32, bind_data.field_names[field_idx],
                                                 bind_data.field_types[field_idx]);
    }
  }

  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_CONJUNCTION) {
    const auto &conj = expr.Cast<duckdb::BoundConjunctionExpression>();
    std::vector<std::shared_ptr<paimon::Predicate>> child_preds;
    for (const auto &child : conj.GetChildren()) {
      if (!child) {
        if (expr.GetExpressionType() == duckdb::ExpressionType::CONJUNCTION_OR) {
          return nullptr;
        }
        continue;
      }
      auto sub = TryBuildPaimonPredicate(get, bind_data, *child);
      if (!sub) {
        if (expr.GetExpressionType() == duckdb::ExpressionType::CONJUNCTION_OR) {
          return nullptr;
        }
        continue;
      }
      child_preds.push_back(std::move(sub));
    }
    if (child_preds.empty()) {
      return nullptr;
    }
    if (child_preds.size() == 1) {
      return child_preds[0];
    }
    if (expr.GetExpressionType() == duckdb::ExpressionType::CONJUNCTION_AND) {
      auto res = paimon::PredicateBuilder::And(child_preds);
      return res.ok() ? res.value() : nullptr;
    }
    if (expr.GetExpressionType() == duckdb::ExpressionType::CONJUNCTION_OR) {
      auto res = paimon::PredicateBuilder::Or(child_preds);
      return res.ok() ? res.value() : nullptr;
    }
  }

  return nullptr;
}

void PaimonPushdownComplexFilter(duckdb::ClientContext &context, duckdb::LogicalGet &get,
                                 duckdb::FunctionData *bind_data_p,
                                 duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &filters) {
  (void)context;
  if (!bind_data_p || filters.empty()) {
    return;
  }
  auto &bind_data = bind_data_p->Cast<PaimonScanFunctionData>();

  std::vector<std::shared_ptr<paimon::Predicate>> predicates;
  // Non-destructive inspection: do NOT erase elements from `filters` so DuckDB keeps
  // its LogicalFilter operator above the scan for exact row-level filtering.
  for (const auto &expr : filters) {
    if (!expr) {
      continue;
    }
    auto pred = TryBuildPaimonPredicate(get, bind_data, *expr);
    if (pred) {
      predicates.push_back(std::move(pred));
    }
  }

  if (predicates.size() == 1) {
    bind_data.scan_predicate = predicates[0];
  } else if (predicates.size() > 1) {
    auto and_res = paimon::PredicateBuilder::And(predicates);
    if (and_res.ok()) {
      bind_data.scan_predicate = and_res.value();
    }
  }
}

duckdb::unique_ptr<duckdb::FunctionData> PaimonBind(
    duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<duckdb::Identifier> &names) {
  if (input.inputs.empty() || input.inputs[0].IsNull()) {
    throw duckdb::BinderException("paimon_scan requires a non-null table_path argument");
  }
  std::string table_path = input.inputs[0].ToString();
  if (table_path.empty()) {
    throw duckdb::BinderException("paimon_scan requires a non-empty table_path argument");
  }

  int64_t snapshot_id = -1;
  if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
    snapshot_id = input.inputs[1].GetValue<int64_t>();
  }
  auto named_it = input.named_parameters.find("snapshot_id");
  if (named_it != input.named_parameters.end() && !named_it->second.IsNull()) {
    snapshot_id = named_it->second.GetValue<int64_t>();
  }

  auto schema_res = LoadPaimonSchemaFromPath(table_path);
  if (!schema_res.ok()) {
    throw duckdb::IOException("paimon_scan: failed to load Paimon schema from '%s': %s",
                              table_path, schema_res.status().ToString());
  }
  auto &paimon_schema = schema_res.value();

  if (snapshot_id >= 0 && table_path.find("://") == std::string::npos) {
    std::error_code ec;
    auto snap_file = std::filesystem::path(table_path) / "snapshot" /
                     ("snapshot-" + std::to_string(snapshot_id));
    if (!std::filesystem::exists(snap_file, ec)) {
      throw duckdb::IOException("paimon_scan: snapshot %lld does not exist at '%s'",
                                static_cast<long long>(snapshot_id), table_path);
    }
  }

  auto arrow_schema_res = paimon_schema->GetArrowSchema();
  if (!arrow_schema_res.ok()) {
    throw duckdb::IOException("paimon_scan: failed to export Arrow schema for '%s': %s",
                              table_path, arrow_schema_res.status().ToString());
  }

  auto factory = duckdb::make_shared_ptr<PaimonScanFactory>();
  factory->table_path = table_path;
  factory->field_names = paimon_schema->FieldNames();

  auto bind_data = duckdb::make_uniq<PaimonScanFunctionData>(factory);
  bind_data->table_path = table_path;
  bind_data->snapshot_id = snapshot_id;
  bind_data->field_names = factory->field_names;
  bind_data->field_types.reserve(bind_data->field_names.size());
  for (const auto &fname : bind_data->field_names) {
    auto ft_res = paimon_schema->GetFieldType(fname);
    bind_data->field_types.push_back(ft_res.ok() ? ft_res.value() : paimon::FieldType::UNKNOWN);
  }

  bind_data->schema_root.arrow_schema = *arrow_schema_res.value();
  arrow_schema_res.value()->release = nullptr;

  duckdb::ArrowTableFunction::PopulateArrowTableSchema(
      context, bind_data->arrow_table, bind_data->schema_root.arrow_schema);
  names = duckdb::StringsToIdentifiers(bind_data->arrow_table.GetNames());
  return_types = bind_data->arrow_table.GetTypes();
  bind_data->all_types = return_types;
  return std::move(bind_data);
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> PaimonInitGlobal(
    duckdb::ClientContext &context, duckdb::TableFunctionInitInput &input) {
  auto &bind_data = input.bind_data->CastNoConst<PaimonScanFunctionData>();

  paimon::ScanContextBuilder scan_builder(bind_data.table_path);
  if (bind_data.snapshot_id >= 0) {
    scan_builder.AddOption(paimon::Options::SCAN_SNAPSHOT_ID,
                           std::to_string(bind_data.snapshot_id));
    scan_builder.AddOption(paimon::Options::SCAN_MODE, "from-snapshot-full");
  }
  if (bind_data.scan_predicate != nullptr) {
    scan_builder.SetPredicate(bind_data.scan_predicate);
  }

  auto scan_ctx_res = scan_builder.Finish();
  if (!scan_ctx_res.ok()) {
    throw duckdb::IOException("paimon_scan: failed to create ScanContext for '%s': %s",
                              bind_data.table_path, scan_ctx_res.status().ToString());
  }
  auto scan_res = paimon::TableScan::Create(std::move(scan_ctx_res).value());
  if (!scan_res.ok()) {
    throw duckdb::IOException("paimon_scan: failed to create TableScan for '%s': %s",
                              bind_data.table_path, scan_res.status().ToString());
  }
  auto plan_res = scan_res.value()->CreatePlan();
  if (!plan_res.ok()) {
    throw duckdb::IOException("paimon_scan: failed to create scan plan for '%s': %s",
                              bind_data.table_path, plan_res.status().ToString());
  }

  bind_data.paimon_factory->splits = plan_res.value()->Splits();
  return duckdb::ArrowTableFunction::ArrowScanInitGlobal(context, input);
}

void ConfigurePaimonTableFunction(duckdb::TableFunction &fn) {
  fn.projection_pushdown = true;
  fn.filter_pushdown = false;
  fn.filter_prune = false;
  fn.pushdown_complex_filter = PaimonPushdownComplexFilter;
}

}  // namespace

void RegisterPaimonScan(duckdb::DatabaseInstance &db) {
  duckdb::TableFunctionSet paimon_scan_set("paimon_scan");

  duckdb::TableFunction fn_one_arg(
      "paimon_scan", {duckdb::LogicalType::VARCHAR},
      duckdb::ArrowTableFunction::ArrowScanFunction, PaimonBind, PaimonInitGlobal,
      duckdb::ArrowTableFunction::ArrowScanInitLocal);
  fn_one_arg.GetSignature().WithTypedKwargs("options", [](duckdb::TypedKwargs &kw) {
    kw.Add("snapshot_id", duckdb::LogicalType::BIGINT);
  });
  ConfigurePaimonTableFunction(fn_one_arg);
  paimon_scan_set.AddFunction(fn_one_arg);

  duckdb::TableFunction fn_two_arg(
      "paimon_scan", {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT},
      duckdb::ArrowTableFunction::ArrowScanFunction, PaimonBind, PaimonInitGlobal,
      duckdb::ArrowTableFunction::ArrowScanInitLocal);
  ConfigurePaimonTableFunction(fn_two_arg);
  paimon_scan_set.AddFunction(fn_two_arg);

  duckdb::CreateTableFunctionInfo info(std::move(paimon_scan_set));
  auto &catalog = duckdb::Catalog::GetSystemCatalog(db);
  auto transaction = duckdb::CatalogTransaction::GetSystemTransaction(db);
  catalog.CreateFunction(transaction, info);
}

}  // namespace myduck::lakehouse
