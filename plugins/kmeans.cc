/** kmeans.cc
    Jeremy Barnes, 16 December 2014
    Copyright (c) 2014 Datacratic Inc.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
    Implementation of an KMEANS algorithm for embedding of a dataset.
*/

#include "mldb/jml/stats/distribution_simd.h"
#include "kmeans.h"
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
#include "mldb/ml/kmeans.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/plugins/sql_config_validator.h"
#include "mldb/server/analytics.h"
#include "mldb/types/any_impl.h"
#include "mldb/types/optional_description.h"
#include "mldb/vfs/fs_utils.h"

using namespace std;


namespace Datacratic {
namespace MLDB {

DEFINE_STRUCTURE_DESCRIPTION(KmeansConfig);

KmeansConfigDescription::
KmeansConfigDescription()
{
    Optional<PolyConfigT<Dataset> > optional;
    optional.emplace(PolyConfigT<Dataset>().
                     withType(KmeansConfig::defaultOutputDatasetType));
    
    addField("trainingData", &KmeansConfig::trainingData,
             "Specification of the data for input to the k-means procedure.  This should be "
             "organized as an embedding, with each selected row containing the same "
             "set of columns with numeric values to be used as coordinates.  The select statement "
             "does not support groupby and having clauses.");
    addField("modelFileUrl", &KmeansConfig::modelFileUrl,
             "URL where the model file (with extension '.kms') should be saved. "
             "This file can be loaded by a function of type 'kmeans' to apply "
             "the trained model to new data. "
             "If someone is only interested in how the training input is clustered "
             "then the parameter can be omitted and the outputDataset param can "
             "be provided instead.");
    addField("outputDataset", &KmeansConfig::output,
             "Dataset for cluster assignment.  This dataset will contain the same "
             "row names as the input dataset, but the coordinates will be replaced "
             "by a single column giving the cluster number that the row was assigned to.",
              optional);
    addField("centroidsDataset", &KmeansConfig::centroids,
             "Dataset in which the centroids will be recorded.  This dataset will "
             "have the same coordinates (columns) as those selected from the input "
             "dataset, but will have one row per cluster, providing the centroid of "
             "the cluster.",
             optional);
    addField("numInputDimensions", &KmeansConfig::numInputDimensions,
             "Number of dimensions from the input to use (-1 = all).  This limits "
             "the number of columns used.  Columns will be ordered alphabetically "
             "and the lowest ones kept.",
             -1);
    addField("numClusters", &KmeansConfig::numClusters,
             "Number of clusters to create.  This will provide the total number of "
             "centroids created.  There must be at least as many rows selected as "
             "clusters.", 10);
    addField("maxIterations", &KmeansConfig::maxIterations,
             "Maximum number of iterations to perform.  If no convergeance is "
             "reached within this number of iterations, the current clustering "
             "will be returned.", 100);
    addField("metric", &KmeansConfig::metric,
             "Metric space in which the k-means distances will be calculated. "
             "Normally this will be Cosine for an orthonormal basis, and "
             "Euclidian for another basis",
             METRIC_COSINE);
    addField("functionName", &KmeansConfig::functionName,
             "If specified, a kmeans function of this name will be created using "
             "the training result.  Note that the 'modelFileUrl' must "
             "also be provided.");
    addParent<ProcedureConfig>();

    onPostValidate = validate<KmeansConfig,
                              InputQuery,
                              MustContainFrom,
                              NoGroupByHaving>(&KmeansConfig::trainingData, "kmeans");
}

// TODO: see http://www.eecs.tufts.edu/~dsculley/papers/fastkmeans.pdf

namespace {

ML::KMeansMetric * makeMetric(MetricSpace metric)
{
    switch (metric) {
    case METRIC_EUCLIDEAN:
        return new ML::KMeansEuclideanMetric();
        break;
    case METRIC_COSINE:
        return new ML::KMeansCosineMetric();
        break;
    default:
        throw ML::Exception("Unknown kmeans metric type");
    }
}

} // file scope

/*****************************************************************************/
/* KMEANS PROCEDURE                                                           */
/*****************************************************************************/

KmeansProcedure::
KmeansProcedure(MldbServer * owner,
               PolyConfig config,
               const std::function<bool (const Json::Value &)> & onProgress)
    : Procedure(owner)
{
    kmeansConfig = config.params.convert<KmeansConfig>();
}

Any
KmeansProcedure::
getStatus() const
{
    return Any();
}

RunOutput
KmeansProcedure::
run(const ProcedureRunConfig & run,
      const std::function<bool (const Json::Value &)> & onProgress) const
{
    auto runProcConf = applyRunConfOverProcConf(kmeansConfig, run);

    // an empty url is allowed but other invalid urls are not
    if(!runProcConf.modelFileUrl.empty() && !runProcConf.modelFileUrl.valid()) {
        throw ML::Exception("modelFileUrl \"" + 
                            runProcConf.modelFileUrl.toString() + "\" is not valid");
    }

    auto onProgress2 = [&] (const Json::Value & progress)
        {
            Json::Value value;
            value["dataset"] = progress;
            return onProgress(value);
        };

    SqlExpressionMldbContext context(server);

    auto embeddingOutput = getEmbedding(*runProcConf.trainingData.stm,
                                        context,
                                        runProcConf.numInputDimensions,
                                        onProgress2);

    std::vector<std::tuple<RowHash, RowName, std::vector<double>,
                           std::vector<ExpressionValue> > > & rows
        = embeddingOutput.first;
    std::vector<KnownColumn> & vars = embeddingOutput.second;

    std::vector<ColumnName> columnNames;
    for (auto & v: vars) {
        columnNames.push_back(v.columnName);
    }

    std::vector<ML::distribution<float> > vecs;

    for (unsigned i = 0;  i < rows.size();  ++i) {
        vecs.emplace_back(ML::distribution<float>(std::get<2>(rows[i]).begin(),
                                                  std::get<2>(rows[i]).end()));
    }

    if (vecs.size() == 0)
        throw HttpReturnException(400, "Kmeans training requires at least 1 datapoint. "
                                  "Make sure your dataset is not empty and that your WHERE expression "
                                  "does not filter all the rows");

    ML::KMeans kmeans;
    kmeans.metric.reset(makeMetric(runProcConf.metric));

    vector<int> inCluster;

    int numClusters = runProcConf.numClusters;
    int numIterations = runProcConf.maxIterations;

    kmeans.train(vecs, inCluster, numClusters, numIterations);

    bool saved = false;
    if (!runProcConf.modelFileUrl.empty()) {
        try {
            Datacratic::makeUriDirectory(runProcConf.modelFileUrl.toString());
            kmeans.save(runProcConf.modelFileUrl.toString());
            saved = true;
        }
        catch (const std::exception & exc) {
            throw HttpReturnException(400, "Error saving kmeans centroids at location'" +
                                      runProcConf.modelFileUrl.toString() + "': " +
                                      exc.what());
        }
    }

    if (runProcConf.output.get()) {

        PolyConfigT<Dataset> outputDataset = *runProcConf.output;
        if (outputDataset.type.empty())
            outputDataset.type = KmeansConfig::defaultOutputDatasetType;

        auto output = createDataset(server, outputDataset, onProgress2, true /*overwrite*/);

        Date applyDate = Date::now();
        
        for (unsigned i = 0;  i < rows.size();  ++i) {
            std::vector<std::tuple<ColumnName, CellValue, Date> > cols;
            cols.emplace_back(ColumnName("cluster"), inCluster[i], applyDate);
            output->recordRow(std::get<1>(rows[i]), cols);
        }
        
        output->commit();
    }

    if (runProcConf.centroids.get()) {

        PolyConfigT<Dataset> centroidsDataset = *runProcConf.centroids;
        if (centroidsDataset.type.empty())
            centroidsDataset.type = KmeansConfig::defaultOutputDatasetType;

        auto centroids = createDataset(server, centroidsDataset, onProgress2, true /*overwrite*/);

        Date applyDate = Date::now();

        for (unsigned i = 0;  i < kmeans.clusters.size();  ++i) {
            auto & cluster = kmeans.clusters[i];

            std::vector<std::tuple<ColumnName, CellValue, Date> > cols;

            for (unsigned j = 0;  j < cluster.centroid.size();  ++j) {
                cols.emplace_back(columnNames[j], cluster.centroid[j], applyDate);
            }
            
            centroids->recordRow(RowName(ML::format("%i", i)), cols);
        }
        
        centroids->commit();
    }

    if(!runProcConf.functionName.empty()) {
        if (saved) {
            KmeansFunctionConfig funcConf;
            funcConf.modelFileUrl = runProcConf.modelFileUrl;

            PolyConfig kmeansFuncPC;
            kmeansFuncPC.type = "kmeans";
            kmeansFuncPC.id = runProcConf.functionName;
            kmeansFuncPC.params = funcConf;

            obtainFunction(server, kmeansFuncPC, onProgress);
        } else {
            throw HttpReturnException(400, "Can't create kmeans function '" +
                                      runProcConf.functionName.rawString() + 
                                      "'. Have you provided a valid modelFileUrl?",
                                      "modelFileUrl", runProcConf.modelFileUrl.toString());
        }
    }

    return Any();
}

DEFINE_STRUCTURE_DESCRIPTION(KmeansFunctionConfig);

KmeansFunctionConfigDescription::
KmeansFunctionConfigDescription()
{
    addField("modelFileUrl", &KmeansFunctionConfig::modelFileUrl,
             "URL of the model file (with extension '.kms') to load. "
             "This file is created by a procedure of type 'kmeans.train'.");

    onPostValidate = [] (KmeansFunctionConfig * cfg, 
                         JsonParsingContext & context) {
        // this includes empty url
        if(!cfg->modelFileUrl.valid()) {
            throw ML::Exception("modelFileUrl \"" + cfg->modelFileUrl.toString() 
                                + "\" is not valid");
        }
    };
}


/*****************************************************************************/
/* KMEANS FUNCTION                                                              */
/*****************************************************************************/

struct KmeansFunction::Impl {
    ML::KMeans kmeans;
    
    Impl(const Url & modelFileUrl) {
        kmeans.load(modelFileUrl.toString());
    }
};

KmeansFunction::
KmeansFunction(MldbServer * owner,
               PolyConfig config,
               const std::function<bool (const Json::Value &)> & onProgress)
    : Function(owner)
{
    functionConfig = config.params.convert<KmeansFunctionConfig>();

    impl.reset(new Impl(functionConfig.modelFileUrl));
    
    dimension = impl->kmeans.clusters[0].centroid.size();

    cerr << "got " << impl->kmeans.clusters.size()
         << " clusters with " << dimension
         << "values" << endl;
}

Any
KmeansFunction::
getStatus() const
{
    return Any();
}

FunctionOutput
KmeansFunction::
apply(const FunctionApplier & applier,
      const FunctionContext & context) const
{
    FunctionOutput result;

    // Extract an embedding with the given column names
    ExpressionValue storage;
    const ExpressionValue & inputVal = context.get("embedding", storage);

    ML::distribution<float> input = inputVal.getEmbedding(dimension);
    Date ts = inputVal.getEffectiveTimestamp();

    int bestCluster = impl->kmeans.assign(input);

    result.set("cluster", ExpressionValue(bestCluster, ts));
    
    return result;
}

FunctionInfo
KmeansFunction::
getFunctionInfo() const
{
    FunctionInfo result;

    result.input.addEmbeddingValue("embedding", dimension);
    result.output.addAtomValue("cluster");

    return result;
}

namespace {

RegisterProcedureType<KmeansProcedure, KmeansConfig>
regKmeans(builtinPackage(),
          "kmeans.train",
          "Simple clustering algorithm based on cluster centroids in embedding space",
          "procedures/KmeansProcedure.md.html");

RegisterFunctionType<KmeansFunction, KmeansFunctionConfig>
regKmeansFunction(builtinPackage(),
                  "kmeans",
                  "Apply a k-means clustering to new data",
                  "functions/Kmeans.md.html");

} // file scope

} // namespace MLDB
} // namespace Datacratic

