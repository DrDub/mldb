
# This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

import json, random, datetime, os

now = datetime.datetime.now()

dataset_config = {
    'type'    : 'sparse.mutable',
    'id'      : 'dataset1',
}
dataset = mldb.create_dataset(dataset_config)
dataset.record_row("row1", [["col1", "a", now]])
dataset.record_row("row2", [["col2", "b", now]])
dataset.commit()

dataset_config["id"] = "dataset2"
dataset = mldb.create_dataset(dataset_config)
dataset.record_row("row1", [["col1", "a", now]])
dataset.record_row("row2", [["col2", "b", now]])
dataset.commit()

dataset_config["id"] = "dataset3"
dataset = mldb.create_dataset(dataset_config)
dataset.record_row("row1", [["col1", "a", now], ["otherRow", "row1", now]])
dataset.record_row("row2", [["col2", "b", now], ["otherRow", "row2", now]])
dataset.commit()


res = mldb.perform("GET", "/v1/query", [["q", """
    SELECT dataset1.rowName(), dataset2.rowName()
    FROM dataset1
    JOIN dataset2 ON dataset2.rowName() = dataset1.rowName()
    """]])

mldb.log(res)
assert res["statusCode"] != 404


res = mldb.perform("GET", "/v1/query", [["q", """
    SELECT dataset1.rowName(), dataset2.rowName(), dataset3.rowName()
    FROM dataset1
    JOIN dataset2 ON dataset2.rowName() = dataset1.rowName()
    JOIN dataset3 ON dataset3.rowName() = dataset1.rowName()
    """]])
mldb.log(res)
assert res["statusCode"] != 404


res = mldb.perform("GET", "/v1/query", [["q", """
    SELECT dataset1.rowName(), dataset2.rowName(), dataset3.rowName()
    FROM dataset1
    JOIN dataset2 ON dataset2.rowName() = dataset1.rowName()
    JOIN dataset3 ON dataset3.rowName() = dataset2.rowName()
    """]])
mldb.log(res)
assert res["statusCode"] != 404



conf = {
    "type": "transform",
    "params": {
        "inputData": """
    SELECT dataset1.rowName(), dataset2.rowName(), dataset3.rowName()
    NAMED dataset2.rowName()
    FROM dataset1
    JOIN dataset2 ON dataset2.rowName() = dataset1.rowName()
    JOIN dataset3 ON dataset3.rowName() = dataset1.rowName()
        """,
        "outputDataset": "output",
        "runOnCreation": True
    }
}
res = mldb.perform("PUT", "/v1/procedures/doit", [], conf)
mldb.log(res)
assert res["statusCode"] != 404

