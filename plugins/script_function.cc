// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/** script_functions.cc
    Francois Maillet, 14 juillet 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

*/

#include "script_function.h"
#include "mldb/server/mldb_server.h"
#include "mldb/types/basic_value_descriptions.h"
#include "mldb/server/function_contexts.h"
#include "mldb/rest/in_process_rest_connection.h"
#include "mldb/types/any_impl.h"


using namespace std;


namespace Datacratic {
namespace MLDB {



/*****************************************************************************/
/* SCRIPT FUNCTION CONFIG                                                    */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(ScriptFunctionConfig);

ScriptFunctionConfigDescription::
ScriptFunctionConfigDescription()
{
    addField("language", &ScriptFunctionConfig::language, 
            "Script language (python or javascript)");
    addField("scriptConfig", &ScriptFunctionConfig::scriptConfig, 
            "Script resource configuration");
}


/*****************************************************************************/
/* SCRIPT FUNCTION                                                           */
/*****************************************************************************/
                      
ScriptFunction::
ScriptFunction(MldbServer * owner,
               PolyConfig config,
               const std::function<bool (const Json::Value &)> & onProgress)
    : Function(owner)
{
    functionConfig = config.params.convert<ScriptFunctionConfig>();

    // Preload the script code in case it's remotly hosted. This was we won't
    // have to download it each time the function is called
    switch(parseScriptLanguage(functionConfig.language)) {
        case PYTHON:        runner = "python";      break;
        case JAVASCRIPT:    runner = "javascript";  break;
        default:
            throw ML::Exception("unknown script language");
    }

    LoadedPluginResource loadedResource(parseScriptLanguage(functionConfig.language),
                        LoadedPluginResource::ScriptType::SCRIPT,
                        "ND", functionConfig.scriptConfig.toPluginConfig());

    cachedResource.source = loadedResource.getScript(PackageElement::MAIN);
}

Any
ScriptFunction::
getStatus() const
{
    return Any();
}

FunctionOutput
ScriptFunction::
apply(const FunctionApplier & applier,
      const FunctionContext & context) const
{
    string resource = "/v1/types/plugins/" + runner + "/routes/run";

    // make it so that if the params parameter contains an args key, we move
    // its contents to the args parameter of the script
    ScriptResource copiedSR(cachedResource);

    ExpressionValue args = context.get<ExpressionValue>("args");
    //Json::Value val = { args.extractJson(), jsonEncode(args.getEffectiveTimestamp()) };
    Json::Value val = jsonEncode(args);
    copiedSR.args = val;

    //cerr << "script args = " << jsonEncode(copiedSR.args) << endl;

    RestRequest request("POST", resource, RestParams(),
                        
                        jsonEncode(copiedSR).toString());
    InProcessRestConnection connection;
    
    // TODO. this should not always be true. need to get this from the context
    // somehow
    request.header.headers.insert(make_pair("__mldb_child_call", "true"));

    server->handleRequest(connection, request);

    // TODO. better exception message
    if(connection.responseCode != 200) {
        throw HttpReturnException(400, "responseCode != 200 for function",
                                  Json::parse(connection.response));
    }

    Json::Value result = Json::parse(connection.response)["result"];
    
    vector<tuple<Coord, ExpressionValue>> vals;
    if(!result.isArray()) {
        throw ML::Exception("Function should return array of arrays.");
    }

    for(const Json::Value & elem : result) {
        if(!elem.isArray() || elem.size() != 3)
            throw ML::Exception("elem should be array of size 3");

        vals.push_back(make_tuple(Coord(elem[0].asString()),
                                  ExpressionValue(elem[1],
                                                  Date::parseIso8601DateTime(elem[2].asString()))));
    }

    FunctionOutput foResult;
    foResult.set("return", vals);
    return foResult;
}

FunctionInfo
ScriptFunction::
getFunctionInfo() const
{

    FunctionInfo result;
    result.input.addRowValue("args");
    result.output.addRowValue("return");

    return result;
}

static RegisterFunctionType<ScriptFunction, ScriptFunctionConfig>
regScriptFunction(builtinPackage(),
                  "script.apply",
                  "Run a JS or Python script as a function",
                  "functions/ScriptApplyFunction.md.html");


} // namespace MLDB
} // namespace Datacratic
