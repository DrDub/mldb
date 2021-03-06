/**
 * ranking_procedure.cc
 * Mich, 2016-01-11
 * This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.
 **/

#include "ranking_procedure.h"
#include "mldb/server/mldb_server.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/server/dataset_context.h"
#include "mldb/types/basic_value_descriptions.h"
#include "mldb/jml/utils/worker_task.h"
#include "mldb/server/function_contexts.h"
#include "mldb/server/bound_queries.h"
#include "mldb/sql/table_expression_operations.h"
#include "mldb/sql/join_utils.h"
#include "mldb/sql/execution_pipeline.h"
#include "mldb/arch/backtrace.h"
#include "mldb/types/any_impl.h"
#include "mldb/server/per_thread_accumulator.h"
#include "mldb/types/date.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/plugins/sql_config_validator.h"
#include <memory>

using namespace std;


namespace Datacratic {
namespace MLDB {

DEFINE_ENUM_DESCRIPTION(RankingType);
RankingTypeDescription::
RankingTypeDescription()
{
    //addValue("percentile", PERCENTILE);
    addValue("index", INDEX);
}

RankingProcedureConfig::
RankingProcedureConfig() :
    rankingType(RankingType::INDEX), rankingColumnName("rank")
{
    outputDataset.withType("sparse.mutable");
}

DEFINE_STRUCTURE_DESCRIPTION(RankingProcedureConfig);

RankingProcedureConfigDescription::
RankingProcedureConfigDescription()
{
    addField("inputData", &RankingProcedureConfig::inputData,
             "An SQL statement to select the input data. The select "
             "expression is required but has no effect. The order by "
             "expression is used to rank the rows.");
    addField("outputDataset", &RankingProcedureConfig::outputDataset,
             "Output dataset configuration. This may refer either to an "
             "existing dataset, or a fully specified but non-existing dataset "
             "which will be created by the procedure.",
             PolyConfigT<Dataset>().withType("sparse.mutable"));
    addField("rankingType", &RankingProcedureConfig::rankingType,
             "The type of the rank to output.");
    addField("rankingColumnName", &RankingProcedureConfig::rankingColumnName,
             "The name to give the ranking column.");
    addParent<ProcedureConfig>();
    onPostValidate = validate<RankingProcedureConfig, 
                              InputQuery, 
                              MustContainFrom>(&RankingProcedureConfig::inputData, "ranking");
}

RankingProcedure::
RankingProcedure(MldbServer * owner,
                 PolyConfig config,
                 const std::function<bool (const Json::Value &)> & onProgress)
    : Procedure(owner)
{
    procedureConfig = config.params.convert<RankingProcedureConfig>();
}

RunOutput
RankingProcedure::
run(const ProcedureRunConfig & run,
    const std::function<bool (const Json::Value &)> & onProgress) const
{
    auto runProcConf = applyRunConfOverProcConf(procedureConfig, run);
    SqlExpressionMldbContext context(server);

    auto boundDataset = runProcConf.inputData.stm->from->bind(context);

    SelectExpression select(SelectExpression::parse("1"));
    vector<shared_ptr<SqlExpression> > calc;

    // We calculate an expression with the timestamp of the order by
    // clause.  First, we need to calculate each of the order by clauses
    for (auto & c: runProcConf.inputData.stm->orderBy.clauses) {
        auto whenClause = std::make_shared<FunctionCallWrapper>
            ("", "when", vector<shared_ptr<SqlExpression> >(1, c.first),
             nullptr /* extract */);
        calc.emplace_back(whenClause);
    }

    vector<Coord> orderedRowNames;
    Date globalMaxOrderByTimestamp = Date::negativeInfinity();
    auto getSize = [&] (NamedRowValue & row,
                        const vector<ExpressionValue> & calc)
    {
        for (auto & c: calc) {
            auto ts = c.getAtom().toTimestamp();
            if (ts.isADate()) {
                globalMaxOrderByTimestamp.setMax(c.getAtom().toTimestamp());
            }
        }

        orderedRowNames.emplace_back(row.rowName);
        return true;
    };

    BoundSelectQuery(select,
                     *boundDataset.dataset,
                     boundDataset.asName,
                     runProcConf.inputData.stm->when,
                     *runProcConf.inputData.stm->where,
                     runProcConf.inputData.stm->orderBy,
                     calc)
        .execute(getSize,
                 runProcConf.inputData.stm->offset,
                 runProcConf.inputData.stm->limit,
                 onProgress);

    int64_t rowCount = orderedRowNames.size();

    auto output = createDataset(server, runProcConf.outputDataset,
                                nullptr, true /*overwrite*/);

    typedef tuple<ColumnName, CellValue, Date> Cell;
    PerThreadAccumulator<vector<pair<RowName, vector<Cell> > > > accum;
    const ColumnName columnName(runProcConf.rankingColumnName);
    function<void(int64_t)> applyFct;
    float countD100 = (rowCount) / 100.0;
    if (false) {
    //if (runProcConf.rankingType == RankingType::PERCENTILE) {
        // Improper implementation, see
        // https://en.wikipedia.org/wiki/Percentile_rank
        applyFct = [&](int64_t idx)
        {
            vector<Cell> rowValue;
            rowValue.emplace_back(columnName,
                                  (idx + 1) / countD100,
                                  globalMaxOrderByTimestamp);

            auto & rows = accum.get();
            rows.emplace_back(orderedRowNames[idx], rowValue);

            if (rows.size() >= 1024) {
                output->recordRows(rows);
                rows.clear();
            }
        };
    }
    else {
        ExcAssert(runProcConf.rankingType == RankingType::INDEX);
        applyFct = [&](int64_t idx)
        {
            vector<Cell> rowValue;
            rowValue.emplace_back(columnName,
                                  idx,
                                  globalMaxOrderByTimestamp);

            auto & rows = accum.get();
            rows.emplace_back(orderedRowNames[idx], rowValue);

            if (rows.size() >= 1024) {
                output->recordRows(rows);
                rows.clear();
            }
        };
    }


    ML::run_in_parallel_blocked(0, rowCount, applyFct);

    // record remainder
    accum.forEach([&] (vector<pair<RowName, vector<Cell> > > * rows)
    {
        output->recordRows(*rows);
    });
    output->commit();
    return output->getStatus();
}

Any
RankingProcedure::
getStatus() const
{
    return Any();
}

static RegisterProcedureType<RankingProcedure, RankingProcedureConfig>
regRankingProcedure(
    builtinPackage(),
    "ranking",
    "Assign ranks over a sorted dataset",
    "procedures/RankingProcedure.md.html");
 

} // namespace MLDB
} // namespace Datacratic
