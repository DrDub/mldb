// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/** script.h                                                   -*- C++ -*-
    Francois Maillet, 10 juillet 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    Script procedure
*/

#pragma once

#include "mldb/types/value_description.h"
#include "mldb/server/plugin_resource.h"
#include "mldb/server/mldb_server.h"
#include "mldb/core/procedure.h"

namespace Datacratic {
namespace MLDB {


/*****************************************************************************/
/* SCRIPT PROCEDURE CONFIG                                                   */
/*****************************************************************************/

struct ScriptProcedureConfig : ProcedureConfig {
    ScriptProcedureConfig()
    {
    }

    std::string language;
    ScriptResource scriptConfig;

    Any args;
};

DECLARE_STRUCTURE_DESCRIPTION(ScriptProcedureConfig);


/*****************************************************************************/
/* SCRIPT PROCEDURE                                                          */
/*****************************************************************************/

struct ScriptProcedure: public Procedure {

    ScriptProcedure(MldbServer * owner,
                    PolyConfig config,
                    const std::function<bool (const Json::Value &)> & onProgress);

    virtual RunOutput run(const ProcedureRunConfig & run,
                          const std::function<bool (const Json::Value &)> & onProgress) const;

    virtual Any getStatus() const;

    ScriptProcedureConfig scriptProcedureConfig;
};

} // namespace MLDB
} // namespace Datacratic
