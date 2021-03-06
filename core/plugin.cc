// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/* plugin.cc
   Jeremy Barnes, 21 January 2014
   Copyright (c) 2014 Datacratic Inc.  All rights reserved.

   Plugin support.
*/

#include "mldb/core/plugin.h"
#include "mldb/server/plugin_collection.h"
#include "mldb/server/mldb_server.h"
#include "mldb/server/static_content_handler.h"
#include "mldb/types/basic_value_descriptions.h"
#include "mldb/rest/rest_request_router.h"
#include <dlfcn.h>
#include <mutex>
#include "mldb/types/any_impl.h"

namespace Datacratic {
namespace MLDB {

/*****************************************************************************/
/* PLUGIN                                                                    */
/*****************************************************************************/

Plugin::
Plugin(MldbServer * server)
    : server(static_cast<MldbServer *>(server))
{
}

Plugin::
~Plugin()
{
}

Any
Plugin::
getStatus() const
{
    return Any();
}
    
Any
Plugin::
getVersion() const
{
    return Any();
}

RestRequestMatchResult
Plugin::
handleRequest(RestConnection & connection,
              const RestRequest & request,
              RestRequestParsingContext & context) const
{
    Json::Value error;
    error["error"] = "Plugin of type '" + ML::type_name(*this)
        + "' does not respond to custom route '" + context.remaining + "'";
    error["details"]["verb"] = request.verb;
    error["details"]["resource"] = request.resource;
    connection.sendErrorResponse(400, error);
    return RestRequestRouter::MR_ERROR;
}

RestRequestMatchResult
Plugin::
handleDocumentationRoute(RestConnection & connection,
                         const RestRequest & request,
                         RestRequestParsingContext & context) const
{
    return RestRequestRouter::MR_NO;
}

/*****************************************************************************/
/* SHARED LIBRARY PLUGIN                                                     */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(SharedLibraryConfig);

SharedLibraryConfigDescription::
SharedLibraryConfigDescription()
{
    addField("address", &SharedLibraryConfig::address,
             "Address to load the shared library code from");
    addField("library", &SharedLibraryConfig::library,
             "Library to load to start plugin");
    addField("doc", &SharedLibraryConfig::doc,
             "Path to serve documentation from");
    addField("apiVersion", &SharedLibraryConfig::apiVersion,
             "Version of the interface required by the shared library");
    addField("version", &SharedLibraryConfig::version,
             "Version of the plugin in this directory");
    addField("allowInsecureLoading", &SharedLibraryConfig::allowInsecureLoading,
             "Allow loading of code that comes from an insecure location", false);
}

static std::mutex dlopenMutex;


struct SharedLibraryPlugin::Itl {
    Itl(SharedLibraryPlugin * owner)
        : owner(owner), handle(nullptr)
          
    {
    }

    ~Itl()
    {
        //dlclose(handle);
    }

    SharedLibraryPlugin * owner;
    void * handle;  ///< Shared library handle
    mutable std::mutex mutex;
    std::shared_ptr<PolyConfig> config;
    SharedLibraryConfig params;
    std::shared_ptr<Plugin> pluginImpl;  // can be null
    
    void load(PolyConfig config)
    {
        params = config.params.convert<SharedLibraryConfig>();

        std::unique_lock<std::mutex> guard(mutex);

        // Also exclude anyone else from using dlopen, since it's not thread
        // safe.
        std::unique_lock<std::mutex> guard2(dlopenMutex);

        if (!params.allowInsecureLoading) {
            throw HttpReturnException
                (400,
                 "Cannot load shared libraries unless allowInsecureLoading is set to true");
        }
        if (params.apiVersion != "1.0.0") {
            throw HttpReturnException
                (400,
                 "Shared library interface version required '"
                 + params.apiVersion
                 + "' doesn't match available 1.0.0");
        }

        //std::string path = "lib" + params.address + ".so";
        std::string path = params.address + params.library;

        dlerror();  // clear existing error
        void * handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            char * error = dlerror();
            ExcAssert(error);
            throw ML::Exception("couldn't find plugin library %s: %s",
                                path.c_str(), error);
        }

        dlerror();  // clear existing error

        auto * fn = (MldbPluginEnterV100 )dlsym(handle, "_Z19mldbPluginEnterV100PN10Datacratic4MLDB10MldbServerE");

        if (fn) {
            Plugin * plugin = fn(owner->server);
            pluginImpl.reset(plugin);
        }
        
        if (!params.doc.empty())
            docHandler = getStaticRouteHandler(params.address + "/" + params.doc,
                                               owner->server);
    }

    RestRequestRouter::OnProcessRequest docHandler;
};

SharedLibraryPlugin::
SharedLibraryPlugin(MldbServer * server,
                    PolyConfig config,
                    std::function<bool (const Json::Value & progress)> onProgress)
    : Plugin(server),
      itl(new Itl(this))
{
    itl->load(config);
}

SharedLibraryPlugin::
~SharedLibraryPlugin()
{
}

Any
SharedLibraryPlugin::
getStatus() const
{
    if (itl->pluginImpl)
        return itl->pluginImpl->getStatus();
    return Any();
}

Any
SharedLibraryPlugin::
getVersion() const
{
    if (itl->pluginImpl)
        return itl->pluginImpl->getVersion();
    return itl->params.version;
}

RestRequestMatchResult
SharedLibraryPlugin::
handleRequest(RestConnection & connection,
              const RestRequest & request,
              RestRequestParsingContext & context) const
{
    if (itl->pluginImpl)
        return itl->pluginImpl->handleRequest(connection, request, context);
    return Plugin::handleRequest(connection, request, context);
}

RestRequestMatchResult
SharedLibraryPlugin::
handleDocumentationRoute(RestConnection & connection,
                         const RestRequest & request,
                         RestRequestParsingContext & context) const
{
    if (itl->docHandler) {
        return itl->docHandler(connection, request, context);
    }
    if (itl->pluginImpl)
        return itl->pluginImpl->handleDocumentationRoute(connection, request, context);
    return MR_NO;
}


RegisterPluginType<SharedLibraryPlugin, SharedLibraryConfig>
regSharedLibrary(builtinPackage(),
                 "sharedLibrary",
                 "Plugin loader for compiled shared libraries",
                 "plugins/SharedLibrary.md.html");

} // namespace MLDB
} // namespace Datacratic

