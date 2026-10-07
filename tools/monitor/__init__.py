"""monitor -- field monitor for deployed DriverPoE units.

Not part of the device_api package, but built entirely on top of it (INFO
only -- no admin secret needed). Polls one or more units' INFO on an
interval and logs every anomalous state transition between two polls, with
enough context to diagnose *why* without being on site. See __main__.py.
"""
