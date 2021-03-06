#
# MLDB-1101-tf-idf.py
# Datacratic, 2015
# This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
#

mldb = mldb_wrapper.wrap(mldb) # noqa

def get_column(result, column, row_index = 0):
    response = result.json()
    for col in response[row_index]['columns']:
        if col[0] == column:
            return col[1]

    assert("could not find column %s on row %d" % (column, row_index))

dataset_config = {
    'type'    : 'sparse.mutable',
    'id'      : "example"
}

dataset = mldb.create_dataset(dataset_config)

#this is our corpus
dataset.record_row(
    "row1", [["test", "peanut butter jelly peanut butter jelly", 0]])
dataset.record_row(
    "row2", [["test", "peanut butter jelly time peanut butter jelly time", 0]])
dataset.record_row(
    "row3", [["test", "this is the jelly song", 0]])

dataset.commit()

# baggify our words
baggify_conf = {
    "type": "transform",
    "params": {
        "inputData": "select tokenize(test, {"
                     "splitchars:' ', quotechar:'', min_token_length: 2}) "
                     "as * from example",
        "outputDataset": {
            "id": "bag_of_words",
            "type": "sparse.mutable"
        },
        "runOnCreation": True
    }
}
mldb.put("/v1/procedures/baggify", baggify_conf)

rez = mldb.get("/v1/query",
               q="select * from bag_of_words order by rowName() ASC")
mldb.log(rez)

# The procedure will count the number of documents each word appears in
tf_idf_conf = {
    "type": "tfidf.train",
    "params": {
        "trainingData": "select * from bag_of_words",
        "modelFileUrl": "file://tmp/MLDB-1101.idf",
        "outputDataset": {
            "id": "tf_idf",
            "type": "sparse.mutable"
        },
        "functionName": "tfidffunction",
        "runOnCreation": True
    }
}

mldb.put("/v1/procedures/tf_idf_proc", tf_idf_conf)

rez = mldb.get("/v1/query", q="select * from tf_idf order by rowName() ASC")
mldb.log(rez)
assert get_column(rez, "peanut") == 2, \
    "expected doc frequency of 2 for 'peanut' in corpus"
assert get_column(rez, "time") == 1, \
    "expected doc frequency of 1 for 'time' in corpus"

rez = mldb.get("/v1/query",
               q="select tfidffunction({{*} as input}) as * "
               "from tf_idf order by rowName() ASC")
mldb.log(rez)
assert get_column(rez, "output.time") > 0, \
    "expected positive tf-idf for 'time'"

def test_relative_values(f_name):
    query = "select %s({tokenize('jelly time butter butter bristol', " \
            "{' ' as splitchars}) as input}) as * from tf_idf" % f_name
    rez = mldb.get("/v1/query", q=query)
    mldb.log(rez)
    assert get_column(rez, "output.bristol") > get_column(rez, 'output.jelly'),  \
        "'bristol' is more relevant than 'jelly' because it shows only in this document"
    assert get_column(rez, "output.butter") >= get_column(rez, 'output.jelly'), \
        "'butter' more relevant than 'jelly' because it occurs moreoften"
    assert get_column(rez, 'output.time') > get_column(rez, 'output.jelly'), \
        "'time' should be more relevant than 'jelly' because its rarer in the corpus"

test_relative_values("tfidffunction")

func_conf = {
    "type": "tfidf",
    "params": {
        "modelFileUrl": "file://tmp/MLDB-1101.idf",
        "tfType" : "augmented",
        "idfType" : "inverseMax",
    }
}

output = mldb.put("/v1/functions/tfidffunction2", func_conf)

test_relative_values("tfidffunction2")

rez = mldb.get("/v1/query",
               q="select tfidffunction2({tokenize('jelly time', "
                 "{' ' as splitchars}) as input}) as * from tf_idf "
                 "order by rowName() ASC")
mldb.log(rez)

#'time' should be more relevant than 'jelly' because its rarer in the corpus
assert get_column(rez, 'output.time') > get_column(rez, 'output.jelly')

mldb.script.set_return("success")
