/* log.h                                                           -*- C++ -*-
   Guy Dumais, 29 January 2016

   This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.

   Logging interface.
*/

#include "log.h"

namespace Datacratic {

namespace MLDB {

static constexpr char const * timestampFormat = "%Y-%m-%dT%T.%e%z";

std::shared_ptr<spdlog::logger> getConfiguredLogger(const std::string & name, const std::string & format) {
    std::shared_ptr<spdlog::logger> logger = spdlog::stdout_logger_mt(name);
    logger->set_pattern(format);
    return logger;
}

std::shared_ptr<spdlog::logger> getQueryLog() {
    static std::shared_ptr<spdlog::logger> queryLog = 
        getConfiguredLogger("query-log", std::string("Q [") + timestampFormat + "] %l %v");
    return queryLog;
}

std::shared_ptr<spdlog::logger> getMldbLog() {
    static std::shared_ptr<spdlog::logger> mldbLog = 
        getConfiguredLogger("mldb", std::string("L [") + timestampFormat + "] %l %v");
    return mldbLog;
}


std::shared_ptr<spdlog::logger> getServerLog() {
    static std::shared_ptr<spdlog::logger> serverLog = 
        getConfiguredLogger("server-log", std::string("A [") + timestampFormat + "] %v");
    return serverLog;
}

// force gcc to export these types
void dummy() {
    (void)getQueryLog();
    (void)getMldbLog();
    (void)getServerLog();
}

} // MLDB

} // Datacratic
