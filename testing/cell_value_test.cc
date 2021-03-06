// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/* cell_value_test.cc                                              -*- C++ -*-
   Jeremy Barnes, 24 December 2014
   Copyright (c) 2014 Datacratic Inc.  All rights reserved.

   Test of cell values.
*/

#include "mldb/sql/cell_value.h"
#include "mldb/types/value_description.h"

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>


using namespace std;
using namespace Datacratic;
using namespace Datacratic::MLDB;

BOOST_AUTO_TEST_CASE( test_size )
{
    BOOST_CHECK_EQUAL(sizeof(CellValue), 16);
}

BOOST_AUTO_TEST_CASE( test_basics )
{
    BOOST_CHECK_EQUAL(CellValue(0), CellValue(0.0));
//    BOOST_CHECK_LT(CellValue(Utf8String("école")), CellValue(Utf8String("zoo")));
    BOOST_CHECK_EQUAL(CellValue(Utf8String("école")), CellValue(Utf8String("école")));
    // since we instrospect the content of the string we will choose the best encoding
    // depending on the contents
    BOOST_CHECK_EQUAL(CellValue(Utf8String("only ascii")).cellType(), CellValue::ASCII_STRING);
    BOOST_CHECK_EQUAL(CellValue(1.0).cellType(), CellValue::INTEGER);
    BOOST_CHECK_EQUAL(CellValue(1.1).cellType(), CellValue::FLOAT);
    //BOOST_CHECK_NO_THROW(CellValue(L"Crédit Agricole Suisse Open Gstaad").cellType());
    //BOOST_CHECK_NO_THROW(CellValue(L"Mutua Madrileña Madrid Open").cellType());
    {
        JML_TRACE_EXCEPTIONS(false);
        BOOST_CHECK_THROW(CellValue(std::string("Crédit Agricole Suisse Open Gstaad")).cellType(), ML::Exception);
        BOOST_CHECK_THROW(CellValue(std::string("Mutua Madrileña Madrid Open")).cellType(),
                          ML::Exception);
    }
    BOOST_CHECK_EQUAL(CellValue(Utf8String("Crédit Agricole Suisse Open Gstaad")).cellType(), CellValue::UTF8_STRING);
    BOOST_CHECK_EQUAL(CellValue(Utf8String("Mutua Madrileña Madrid Open")).cellType(), CellValue::UTF8_STRING);
    BOOST_CHECK_EQUAL(CellValue().cellType(), CellValue::EMPTY);
    BOOST_CHECK_EQUAL(CellValue("").cellType(), CellValue::ASCII_STRING);
    BOOST_CHECK_EQUAL(CellValue("1").cellType(), CellValue::ASCII_STRING);
    BOOST_CHECK_EQUAL(CellValue("-1").cellType(), CellValue::ASCII_STRING);
    BOOST_CHECK_EQUAL(CellValue("+1").cellType(), CellValue::ASCII_STRING);
    BOOST_CHECK_NE(CellValue(), CellValue(""));
    BOOST_CHECK_NE(CellValue(0), CellValue("0"));
    BOOST_CHECK_NE(CellValue(0), CellValue("0.0"));
    BOOST_CHECK_NE(CellValue(0), CellValue("+0.0"));
    BOOST_CHECK_NE(CellValue(0), CellValue("-0.0"));
    BOOST_CHECK_LT(CellValue(-1), CellValue(0));
    BOOST_CHECK_LT(CellValue(), CellValue(0));
    BOOST_CHECK_LT(CellValue(1), CellValue(1.1));
    
    auto nan = std::numeric_limits<float>::quiet_NaN();

    BOOST_CHECK_EQUAL(CellValue(nan), CellValue(nan));
    BOOST_CHECK_LT(CellValue(nan), CellValue(0));
    BOOST_CHECK(!(CellValue(nan) < CellValue(nan)));
    
    // Corner cases that should be tested:
    // 1.  Not a number, including case variants
    // 2.  Infinities, including case variants
    // 3.  64 bit integers that can't be exactly represented by a double
    // 4.  numbers between LONG_LONG_MAX and ULONG_LONG_MAX
    // 5.  Scientific notation
}

void checkOrdering(const CellValue & v1,
                   const CellValue & v2)
{
    bool eq = v1 == v2;
    bool lt = v1 <  v2;
    bool gt = v2 <  v1;

    if (eq)
        BOOST_CHECK_EQUAL(v1.toString(), v2.toString());

    if (eq + lt + gt != 1) {
        cerr << "ordering error with " << v1 << " and " << v2 << endl;
    }

    BOOST_CHECK_EQUAL(eq + lt + gt, 1);
}

BOOST_AUTO_TEST_CASE( test_ordering )
{
    vector<CellValue> values { CellValue(), std::numeric_limits<float>::quiet_NaN(), -1.5, 1, 2, 2.3, 3, 3.0, 3.5, "", "one", "three", "three hundred and forty-five thousand", "two" };

    for (auto & v1: values) {
        for (auto & v2: values) {
            checkOrdering(v1, v2);
        }
    }

    for (unsigned i = 0;  i < 10;  ++i) {
        vector<CellValue> unsorted = values;
        std::random_shuffle(unsorted.begin(), unsorted.end());
        std::sort(unsorted.begin(), unsorted.end());

        BOOST_CHECK_EQUAL_COLLECTIONS(unsorted.begin(), unsorted.end(),
                                      values.begin(), values.end());
    }
}

BOOST_AUTO_TEST_CASE( test_printing )
{
    BOOST_CHECK_NE(CellValue("1.100000000"), CellValue("1.1"));
    BOOST_CHECK_EQUAL(CellValue("1.1").toString(), "1.1");
    BOOST_CHECK_EQUAL(CellValue("-1.1").toString(), "-1.1");
    BOOST_CHECK_EQUAL(CellValue("1.1e100").toString(), "1.1e100");
    BOOST_CHECK_EQUAL(CellValue("1e100").toString(), "1e100");
    BOOST_CHECK_EQUAL(CellValue("1.1e-100").toString(), "1.1e-100");
    BOOST_CHECK_EQUAL(CellValue("1e-100").toString(), "1e-100");
    BOOST_CHECK_EQUAL(CellValue("0.1").toString(), "0.1");
    BOOST_CHECK_EQUAL(CellValue("0.01").toString(), "0.01");

    BOOST_CHECK_EQUAL(CellValue("long long long long").toString(), "long long long long");
}

BOOST_AUTO_TEST_CASE( test_64_bit_range )
{
    BOOST_CHECK_EQUAL(CellValue(std::numeric_limits<uint64_t>::max()).toUInt(),
                      std::numeric_limits<uint64_t>::max());
    {
        JML_TRACE_EXCEPTIONS(false);
        auto v = [&] () { return CellValue(std::numeric_limits<uint64_t>::max()).toInt(); };
        BOOST_CHECK_THROW(v(), ML::Exception);
    }
}

BOOST_AUTO_TEST_CASE( test_date )
{
    BOOST_CHECK_EQUAL(CellValue(Date::fromSecondsSinceEpoch(0.0)).toString(),
                      "1970-01-01T00:00:00Z");
    BOOST_CHECK_EQUAL(CellValue(Date::fromSecondsSinceEpoch(0.1)).toString(),
                      "1970-01-01T00:00:00.1Z");
    BOOST_CHECK_EQUAL(CellValue(Date::fromSecondsSinceEpoch(0.002)).toString(),
                      "1970-01-01T00:00:00.002Z");
    BOOST_CHECK_EQUAL(CellValue(Date::fromSecondsSinceEpoch(0.333)).toString(),
                      "1970-01-01T00:00:00.333Z");

    Date now(Date::now());
    CellValue ts1(now);
    cerr << "ts1.toString() = " << ts1.toString() << endl;
    BOOST_CHECK_EQUAL(CellValue(ts1.toString()).coerceToTimestamp(),
                      ts1);

    BOOST_CHECK_EQUAL(jsonDecode<CellValue>(jsonEncode(ts1)),
                      ts1);

    BOOST_CHECK_EQUAL(CellValue(jsonDecodeStr<Date>(string("\"2015-10-06T20:52:18.842Z\""))).toString(),
                      "2015-10-06T20:52:18.842Z");

    BOOST_CHECK_EQUAL(jsonEncodeStr(CellValue(jsonDecodeStr<Date>(string("\"2015-10-06T20:52:18.842Z\"")))),
                      "{\"ts\":\"2015-10-06T20:52:18.842Z\"}");
}

BOOST_AUTO_TEST_CASE(test_blob)
{
    char blobData[] = "\1\1\2\3\4\5\0";
    string blobContents(blobData, blobData + 7);

    BOOST_CHECK_EQUAL(blobContents.size(), 7);

    CellValue blob = CellValue::blob(blobContents);
    
    BOOST_CHECK_EQUAL(blob.cellType(), CellValue::BLOB);
    BOOST_CHECK(blob.isBlob());
    BOOST_CHECK_EQUAL(blob, blob);
    BOOST_CHECK(!(blob != blob));
    BOOST_CHECK(!(blob < blob));
    
    BOOST_CHECK_EQUAL(blob.blobLength(), blobContents.size());
    BOOST_CHECK_EQUAL(string(blob.blobData(), blob.blobData() + blob.blobLength()),
                      blobContents);

    BOOST_CHECK_EQUAL(jsonEncodeStr(blob), "{\"blob\":[1,1,2,3,4,5,0]}");
    BOOST_CHECK_EQUAL(jsonEncodeStr(CellValue::blob("")),
                      "{\"blob\":[]}");
    BOOST_CHECK_EQUAL(jsonEncodeStr(jsonDecode<CellValue>(jsonEncode(CellValue::blob("")))),
                      jsonEncodeStr(CellValue::blob("")));
    BOOST_CHECK_EQUAL(jsonEncodeStr(CellValue::blob("hello\1")),
                      "{\"blob\":[\"hello\",1]}");
    BOOST_CHECK_EQUAL(jsonEncodeStr(jsonDecode<CellValue>(jsonEncode(CellValue::blob("hello\1")))),
                      jsonEncodeStr(CellValue::blob("hello\1")));

    BOOST_CHECK_EQUAL(jsonEncodeStr(jsonDecodeStr<CellValue>(jsonEncodeStr(blob))),
                      jsonEncodeStr(blob));
}
