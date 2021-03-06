# Notebooks and pymldb

## Notebooks

The built-in Notebook interface is a standard Jupyter installation, with a number of useful packages preinstalled such as numpy, pandas, matplotlib etc (see [list](licenses.md)). In addition, the `pymldb` library (see below) comes pre-installed, which makes [working with the REST API](WorkingWithRest.md) easier. Everything that can be done in the MLDB-hosted Notebook interface can be done by installing Jupyter and all the relevant modules (including `pymldb` via `pip install pymldb`) somewhere else, like on a workstation or another server.

## The `pymldb` library

The [`pymldb` library](http://github.com/datacratic/pymldb)  is an open-source pure-Python module, installable via `pip install pymldb`, which provides a wrapper library that makes it easy to work with MLDB from Python. Check out the ![](%%nblink _tutorials/Using pymldb Tutorial) notebook for more info.