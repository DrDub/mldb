#
# MLDB-1119_pooling_function.py
# Datacratic, 2016
# This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.
#
import datetime

mldb = mldb_wrapper.wrap(mldb) # noqa

# Create embedding dataset
dataset_config = {
    'type'    : 'embedding',
    'id'      : 'wordEmbedding'
}

dataset = mldb.create_dataset(dataset_config)
now = datetime.datetime.now()

dataset.record_row("allo", [["x", 0.2, now], ["y", 0, now]])
dataset.record_row("mon",  [["x", 0.8, now], ["y", 0.95, now]])
dataset.record_row("beau", [["x", 0.4, now], ["y", 0.01, now]])
dataset.record_row("coco", [["x", 0, now],   ["y", 0.5, now]])
dataset.commit()


# Create bag of words dataset
dataset_config = {
    'type'    : 'sparse.mutable',
    'id'      : 'bag_o_words'
}

dataset = mldb.create_dataset(dataset_config)

dataset.record_row("doc1",  [["allo", 1, now], ["coco", 1, now]])
dataset.record_row("doc2",  [["allo", 1, now], ["mon", 1, now],
                             ["beau", 1, now]])
dataset.record_row("doc3",  [["patate", 1, now]])
dataset.record_row("doc4",  [["j'ai", 1, now]])
dataset.commit()


# create pooling function
conf = {
    "type": "pooling",
    "params": {
        "embeddingDataset": "wordEmbedding",
        "aggregators": ["avg", "max"]
    }
}
res = mldb.put("/v1/functions/poolz", conf)
mldb.log(res.json())

res = mldb.get(
    "/v1/query",
    q="select poolz({words: {*}})[embedding] as word2vec from bag_o_words")
js_res = res.json()
mldb.log(js_res)


def assert_val(res, rowName, colName, value):
    for row in res:
        if row["rowName"] != rowName:
            continue

        for col in row["columns"]:
            if col[0] == colName:
                assert abs(col[1] - value) < 0.0001
                return True

        # did not find col
        assert False

    # did not find row
    assert False

# max of x dim for allo or coco
assert_val(js_res, "doc1", "word2vec.2", 0.2)
# avg of y dim for allo, mon, beau
assert_val(js_res, "doc2", "word2vec.1", 0.32)
# no match
assert_val(js_res, "doc4", "word2vec.0", 0)

mldb.script.set_return("success")
