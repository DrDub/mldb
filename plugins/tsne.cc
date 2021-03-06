/** tsne.cc
    Jeremy Barnes, 16 December 2014
    Copyright (c) 2014 Datacratic Inc.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

    Implementation of an TSNE algorithm for embedding of a dataset.
*/

#include "tsne.h"
#include "matrix.h"
#include "mldb/server/mldb_server.h"
#include "mldb/core/dataset.h"
#include "mldb/jml/stats/distribution.h"
#include <boost/multi_array.hpp>
#include "mldb/jml/utils/guard.h"
#include "mldb/jml/utils/worker_task.h"
#include "mldb/jml/utils/pair_utils.h"
#include "mldb/arch/timers.h"
#include "mldb/arch/simd_vector.h"
#include "mldb/jml/utils/vector_utils.h"
#include "mldb/types/basic_value_descriptions.h"
#include "mldb/ml/value_descriptions.h"
#include "mldb/types/any_impl.h"
#include "mldb/plugins/sql_config_validator.h"
#include "mldb/ml/tsne/vantage_point_tree.h"
#include "mldb/ml/tsne/tsne.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/server/analytics.h"
#include "mldb/vfs/fs_utils.h"
#include "mldb/vfs/filter_streams.h"
#include "mldb/types/jml_serialization.h"

using namespace std;


namespace Datacratic {
namespace MLDB {

DEFINE_STRUCTURE_DESCRIPTION(TsneConfig);

TsneConfigDescription::
TsneConfigDescription()
{
    addField("trainingData", &TsneConfig::trainingData,
             "Specification of the data for input to the TSNE procedure.  This should be "
             "organized as an embedding, with each selected row containing the same "
             "set of columns with numeric values to be used as coordinates.  The select statement "
             "does not support groupby and having clauses.");
    addField("rowOutputDataset", &TsneConfig::output,
             "Dataset for TSNE output, with embeddings of training data. "
             "One row will be added for each row in the input dataset, "
             "with a list of coordinates",
             PolyConfigT<Dataset>().withType("embedding"));
    addField("modelFileUrl", &TsneConfig::modelFileUrl,
             "URL where the t-SNE model file (with extension '.tsn') should be saved. "
             "This file can be loaded by a function of type 'tsne.embedRow'.");
    addField("numInputDimensions", &TsneConfig::numInputDimensions,
             "Number of dimensions from the input to use.  This will limit "
             "the columns to the n first columns in the alphabetical "
             "sorting of the columns (-1 = all)",
             -1);
    addField("numOutputDimensions", &TsneConfig::numOutputDimensions,
             "Number of dimensions to produce in t-SNE space.  Normally "
             "this will be 2 or 3, depending upon the number of dimensions "
             "in the visualization");
    addField("tolerance", &TsneConfig::tolerance,
             "Tolerance of perplexity calculation.  This is an internal "
             "parameter that only needs to be changed in rare circumstances.");
    addField("perplexity", &TsneConfig::perplexity,
             "Perplexity to aim for; higher means more spread out.  This "
             "controls how hard t-SNE tries to spread the points out.  If "
             "the resulting output looks more like a ball or a sphere than "
             "individual clusters, you should reduce this number.  If it "
             "looks like a dot or star, you should increase it.");
    addField("functionName", &TsneConfig::functionName,
             "If specified, an tsne.embedRow function of this name will be "
             "created using the trained model.");
    addParent<ProcedureConfig>();

    onPostValidate = validate<TsneConfig,
                              InputQuery,
                              MustContainFrom,
                              NoGroupByHaving>(&TsneConfig::trainingData, "tsne");
}


/*****************************************************************************/
/* TSNE PROCEDURE                                                             */
/*****************************************************************************/

struct TsneItl {
    TsneItl()
    {
    }

    TsneItl(const Url & filename)
    {
        ML::filter_istream stream(filename.toString());
        ML::DB::Store_Reader store(stream);

        reconstitute(store);

        stream.close();
    }
    
    ML::TSNE_Params params;
    boost::multi_array<float, 2> inputCoords;
    boost::multi_array<float, 2> outputCoords;
    std::unique_ptr<ML::VantagePointTree> vpTree;
    std::unique_ptr<ML::Quadtree> qtree;
    std::vector<Utf8String> inputColumnNames;
    std::vector<Utf8String> outputColumnNames;
    std::shared_ptr<const std::vector<ColumnName> > outputColumnNamesShared;

    size_t numOutputDimensions() const { return outputCoords.shape()[1]; }

    int64_t memusage() const
    {
        int64_t result = sizeof(*this);
        result += sizeof(float) * inputCoords.shape()[0] * inputCoords.shape()[1];
        result += sizeof(float) * outputCoords.shape()[0] * outputCoords.shape()[1];
        result += vpTree->memusage();
        result += qtree->root->memusage();
        return result;
    }

    void save(const std::string & filename) const
    {
        ML::filter_ostream stream(filename);
        ML::DB::Store_Writer store(stream);
        serialize(store);
    }

    void serialize(ML::DB::Store_Writer & store) const
    {
        using namespace ML::DB;

        store << string("TSNE") << compact_size_t(2);
        
        size_t rows = inputCoords.shape()[0];
        size_t dimsIn = inputCoords.shape()[1];
        size_t dimsOut = outputCoords.shape()[1];


        store << compact_size_t(rows)
              << compact_size_t(dimsIn)
              << compact_size_t(dimsOut);
        
        store << inputCoords << outputCoords;

        store << inputColumnNames << outputColumnNames;

        ML::VantagePointTree::serializePtr(store, vpTree.get());
        qtree->serialize(store);
    }

    void reconstitute(ML::DB::Store_Reader & store)
    {
        using namespace ML::DB;

        string tag;
        store >> tag;
        if (tag != "TSNE")
            throw ML::Exception("Expected TSNE tag");
        compact_size_t version(store);
        if (version != 2)
            throw ML::Exception("Unknown version for t-SNE");

        compact_size_t rows(store), dimsIn(store), dimsOut(store);

        store >> inputCoords >> outputCoords;

        store >> inputColumnNames >> outputColumnNames;

        outputColumnNamesShared.reset(new vector<ColumnName>(outputColumnNames.begin(), outputColumnNames.end()));

        vpTree.reset(ML::VantagePointTree::reconstitutePtr(store));
        qtree.reset(new ML::Quadtree(store));
    }

    ML::distribution<float> reembed(const ML::distribution<float> & v) const
    {
        return retsneApproxFromCoords(v, inputCoords, outputCoords,
                                      *qtree, *vpTree, params);
    }

};

TsneProcedure::
TsneProcedure(MldbServer * owner,
            PolyConfig config,
            const std::function<bool (const Json::Value &)> & onProgress)
    : Procedure(owner)
{
    tsneConfig = config.params.convert<TsneConfig>();
}

Any
TsneProcedure::
getStatus() const
{
    return Any();
}

RunOutput
TsneProcedure::
run(const ProcedureRunConfig & run,
      const std::function<bool (const Json::Value &)> & onProgress) const
{
    auto runProcConf = applyRunConfOverProcConf(tsneConfig, run);

    auto onProgress2 = [&] (const Json::Value & progress)
        {
            Json::Value value;
            value["dataset"] = progress;
            return onProgress(value);
        };

    // 1.  Get a numsubjects by numdimensions input matrix to train the
    //     t-SNE algorithm on
    auto itl = std::make_shared<TsneItl>();


    itl->params.perplexity = runProcConf.perplexity;
    itl->params.tolerance = runProcConf.tolerance;

    //cerr << "perplexity = " << itl->params.perplexity << endl;
    //cerr << "tolerance = " << itl->params.tolerance << endl;

    //cerr << "doing t-SNE" << endl;


    SqlExpressionMldbContext context(server);

    //cerr << "numDims = " << numDims << endl;

    auto embeddingOutput = getEmbedding(*runProcConf.trainingData.stm,
                                        context,
                                        runProcConf.numInputDimensions,
                                        onProgress2);

    std::vector<std::tuple<RowHash, RowName, std::vector<double>,
                           std::vector<ExpressionValue> > > & rows
        = embeddingOutput.first;
    std::vector<KnownColumn> & vars = embeddingOutput.second;

    size_t numDims = vars.size();

    boost::multi_array<float, 2> coords
        (boost::extents[rows.size()][numDims]);

    for (unsigned i = 0;  i < rows.size();  ++i) {
        for (auto & e: std::get<2>(rows[i]))
            ExcAssert(isfinite(e));
        std::copy(std::get<2>(rows[i]).begin(), std::get<2>(rows[i]).end(),
                  &coords[i][0]);
    }

    if (coords.size() == 0)
        throw HttpReturnException(400, "t-sne training requires at least 1 datapoint. "
                                  "Make sure your dataset is not empty and that your WHERE, offset "
                                  "and limit expressions do not filter all the rows");

//cerr << "copied into matrix" << endl;

#if 0
    cerr << "rows[0] = " << rows[0].second << endl;
    cerr << "rows[1] = " << rows[1].second << endl;
    cerr << "rows[2] = " << rows[1].second << endl;
    cerr << "rows[0] dot rows[1] = " << rows[0].second.dotprod(rows[1].second)
         << endl;
    cerr << "rows[0] dot rows[2] = " << rows[0].second.dotprod(rows[2].second)
         << endl;
    cerr << "rows[0] dist rows[1] = " << (rows[0].second - rows[1].second).two_norm()
         << endl;
    cerr << "rows[0] dist rows[2] = " << (rows[0].second - rows[2].second).two_norm()
         << endl;
    cerr << "rows[1] dist rows[2] = " << (rows[1].second - rows[2].second).two_norm()
         << endl;
#endif

    itl->inputCoords.resize(boost::extents[rows.size()][numDims]);
    itl->inputCoords = coords;

    ML::TSNE_Callback callback = [&] (int iter, float cost,
                                      std::string phase)
        {
            if (iter == 1 || iter % 10 == 0)
                cerr << "phase " << phase << " iter " << iter
                     << " cost " << cost << endl;
            return true;
        };

    ExcAssertGreaterEqual(runProcConf.numOutputDimensions, 1);

    itl->outputCoords.resize(boost::extents[rows.size()][runProcConf.numOutputDimensions]);
    itl->outputCoords
        = ML::tsneApproxFromCoords(coords, runProcConf.numOutputDimensions,
                                   itl->params, callback, &itl->vpTree,
                                   &itl->qtree);
    
    ExcAssert(itl->qtree);
    ExcAssert(itl->vpTree);

    vector<ColumnName> names = { ColumnName("x"), ColumnName("y"), ColumnName("z") };
    if (runProcConf.numOutputDimensions <= 3) 
        names.resize(runProcConf.numOutputDimensions);
    else {
        names.clear();
        for (unsigned i = 0; i < runProcConf.numOutputDimensions;  ++i)
            names.push_back(ColumnName(ML::format("dim%04d", i)));
    }


    // Record the column names for later
    for (auto & c: vars) {
        itl->inputColumnNames.push_back(c.columnName.toUtf8String());
    }
    for (auto & c: names) {
        itl->outputColumnNames.push_back(c.toUtf8String());
    }

    itl->outputColumnNamesShared
        .reset(new vector<ColumnName>(itl->outputColumnNames.begin(),
                                      itl->outputColumnNames.end()));
    
    if (!runProcConf.modelFileUrl.empty()) {
        Datacratic::makeUriDirectory(runProcConf.modelFileUrl.toString());
        itl->save(runProcConf.modelFileUrl.toString());
    }

    // Create a dataset to contain the output embedding if we ask for it
    if (!runProcConf.output.type.empty()) {
        auto output = createDataset(server, runProcConf.output, onProgress2, true /*overwrite*/);

        for (unsigned i = 0;  i < rows.size();  ++i) {
            //cerr << "row " << i << " had coords " << itl->outputCoords[i][0] << ","
            //     << itl->outputCoords[i][1] << endl;
            std::vector<std::tuple<ColumnName, CellValue, Date> > cols;
            for (unsigned j = 0;  j < runProcConf.numOutputDimensions;  ++j) {
                ExcAssert(isfinite(itl->outputCoords[i][j]));
                cols.emplace_back(names[j], itl->outputCoords[i][j], Date());
            }

            output->recordRow(std::get<1>(rows[i]), cols);
        }

        output->commit();
    }
    
    if(!runProcConf.functionName.empty()) {
        PolyConfig tsneFuncPC;
        tsneFuncPC.type = "tsne.embedRow";
        tsneFuncPC.id = runProcConf.functionName;
        tsneFuncPC.params = TsneEmbedConfig(runProcConf.modelFileUrl);

        obtainFunction(server, tsneFuncPC, onProgress);
    }

    return Any();
}


DEFINE_STRUCTURE_DESCRIPTION(TsneEmbedConfig);

TsneEmbedConfigDescription::
TsneEmbedConfigDescription()
{
    addField("modelFileUrl", &TsneEmbedConfig::modelFileUrl,
             "URL of the model file (with extension '.tsn') to load. "
             "This file is created by a procedure of type 'tsne.train'.");
}


/*****************************************************************************/
/* TSNE EMBED ROW                                                            */
/*****************************************************************************/

TsneEmbed::
TsneEmbed(MldbServer * owner,
          PolyConfig config,
          const std::function<bool (const Json::Value &)> & onProgress)
    : Function(owner)
{
    functionConfig = config.params.convert<TsneEmbedConfig>();
    itl = std::make_shared<TsneItl>(functionConfig.modelFileUrl);
}

Any
TsneEmbed::
getStatus() const
{
    return Any();
}

FunctionOutput
TsneEmbed::
apply(const FunctionApplier & applier,
      const FunctionContext & context) const
{
    throw HttpReturnException(500, "t-sne application is not implemented yet");
    
    FunctionOutput result;

    ExpressionValue storage;
    const ExpressionValue & inputVal = context.get("embedding", storage);
    ML::distribution<float> input = inputVal.getEmbedding(itl->inputColumnNames.size());

    Date ts = Date::negativeInfinity();
    auto embedding = itl->reembed(input);

    result.set("tsne", ExpressionValue(embedding, ts));
    
    return result;
}

FunctionInfo
TsneEmbed::
getFunctionInfo() const
{
    FunctionInfo result;
    
    result.input.addEmbeddingValue("embedding", itl->inputColumnNames.size());
    result.output.addEmbeddingValue("tsne", itl->numOutputDimensions());
    
    return result;
}

namespace {

RegisterProcedureType<TsneProcedure, TsneConfig>
regTsne(builtinPackage(),
        "tsne.train",
        "Project a high dimensional space into a low-dimensional space suitable for visualization",
        "procedures/TsneProcedure.md.html");


RegisterFunctionType<TsneEmbed, TsneEmbedConfig>
regTsneEmbed(builtinPackage(),
             "tsne.embedRow",
             "Embed a pre-trained t-SNE algorithm to new data points",
             "functions/TsneEmbed.md.html",
             nullptr,
             {MldbEntity::INTERNAL_ENTITY});

} // file scope

} // namespace MLDB
} // namespace Datacratic

