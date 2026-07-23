#pragma once
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include <exception>

#define AGENTXX_CATCH_EXCEPTION_D(errInfo, code)           \
    catch (const std::exception& e) {                      \
        {code} errInfo = e.what();                         \
    }                                                      \
    catch (const boost::exception& e) {                    \
        {code} errInfo = boost::diagnostic_information(e); \
    }                                                      \
    catch (...) {                                          \
        code                                               \
    }
