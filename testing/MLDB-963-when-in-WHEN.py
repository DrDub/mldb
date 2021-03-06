#
# MLDB-963-when-in-WHEN.py
# Datacratic, 2015
# This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
#

import unittest
import datetime

mldb = mldb_wrapper.wrap(mldb) # noqa
now = datetime.datetime.now()

class WhenInWhen(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        same_time_tomorrow = now + datetime.timedelta(days=1)
        ds1 = mldb.create_dataset({
            'type': 'sparse.mutable',
            'id': 'dataset1'})

        row_count = 10
        for i in xrange(row_count - 1):
            # row name is x's value
            ds1.record_row(str(i),
                           [['x', i, same_time_tomorrow], ['y', i, now]])

        ds1.record_row(str(row_count - 1), [['x', 9, same_time_tomorrow],
                                            ['y', 9, same_time_tomorrow]])
        ds1.commit()

    def test_1(self):
        def validate1(result):
            mldb.log(result)
            for row in result.json():
                if row['rowName'] != 9:
                    self.assertEqual(len(row["columns"]), 1,
                                     'expected x to be filtered out')
                else:
                    self.assertTrue('columns ' not in row,
                                    'expected x and y to be filtered out')

        validate1(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN timestamp() < when(x)"))
        validate1(mldb.get('/v1/datasets/dataset1/query',
                           when="timestamp() < when(x)"))

    def test_2(self):
        def validate2(result):
            mldb.log(result)
            rows = result.json()
            msg = 'expected where clause to filter all but row 9'
            self.assertEqual(len(rows), 1, msg)
            self.assertEqual(rows[0]['rowName'], 9, msg)
            self.assertEqual(
                len(rows[0]['columns']), 2,
                'expected the two tuples to be preserved by WHEN clause')

        validate2(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN timestamp() = when(x) WHERE x = 9"))
        validate2(mldb.get('/v1/datasets/dataset1/query',
                           when='timestamp() = when(x)', where='x = 9'))

    def tesst_3(self):
        def validate3(result):
            mldb.log(result)
            rows = result.json()
            for row in rows:
                if row['rowName'] != 9:
                    self.assertEqual(len(row["columns"]), 1,
                                     'expected y to be filtered out')
                else:
                    self.assertEqual(len(row["columns"]), 2,
                                     'expected x and y to be preserved')

        validate3(mldb.get(
            '/v1/query', q="SELECT * FROM dataset1 WHEN timestamp() > now()"))
        validate3(mldb.get('/v1/datasets/dataset1/query',
                           when='timestamp() > now()'))

    def test_4(self):
        def validate4(result):
            mldb.log(result)
            rows = result.json()
            for row in rows:
                if row['rowName'] != 9:
                    self.assertEqual(len(row["columns"]), 1,
                                     'expected y to be filtered out')
                else:
                    self.assertEqual(len(row["columns"]), 2,
                                     'expected x and y to be preserved')

        validate4(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN timestamp() BETWEEN now() AND "
              "now() + INTERVAL '1W'"))
        validate4(mldb.get(
            '/v1/datasets/dataset1/query',
            when="timestamp() BETWEEN now() AND now() + INTERVAL '1W'"))

    def test_5(self):
        def validate5(result):
            mldb.log(result)
            rows = result.json()
            for row in rows:
                self.assertEqual(len(row["columns"]), 2,
                                 'expected x and y to be preserved')

        validate5(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN timestamp() "
              "BETWEEN now() - INTERVAL '1d' AND when({*})"))
        validate5(mldb.get(
            '/v1/datasets/dataset1/query',
            when="timestamp() BETWEEN now() - INTERVAL '1d' AND when({*})"))

    def test_6(self):
        def validate6(result):
            mldb.log(result)
            rows = result.json()
            for row in rows:
                self.assertTrue('columns' not in row,
                                'expected all values to be filtered out')

        validate6(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN timestamp() "
              "BETWEEN when({*}) + INTERVAL '1s' AND '2026-01-01'"))
        validate6(mldb.get(
            '/v1/datasets/dataset1/query',
            when="timestamp() BETWEEN when({*}) + INTERVAL '1s' AND '2026-01-01'"))

    def test_7(self):
        def validate7(result):
            mldb.log(result)
            rows = result.json()
            for row in rows:
                if row['rowName'] != 9:
                    self.assertTrue('columns' not in row,
                                    'expected x and y to be filtered out')
                else:
                    self.assertEqual(len(row["columns"]), 2,
                                     'expected x and y to be preserved')

        validate7(mldb.get(
            '/v1/query',
            q="SELECT * FROM dataset1 WHEN when(y) > to_timestamp('%s') + INTERVAL '2s'" % now))
        validate7(mldb.get('/v1/datasets/dataset1/query',
                           when="when(y) > to_timestamp('%s') + INTERVAL '2s'" % now))

if __name__ == '__main__':
    mldb.run_tests()
