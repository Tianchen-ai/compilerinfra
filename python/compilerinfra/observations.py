"""Bounded history shared by a JIT handle and the host action catalog."""

from collections import OrderedDict


class Observations(OrderedDict):
    limit = 128

    def __setitem__(self, key, value):
        super().__setitem__(key, value)
        if len(self) > self.limit:
            self.popitem(last=False)
