import logging as log
import numpy as np
import enum
from collections import defaultdict


class ScannerException(Exception):
    pass


class DeviceType(enum.Enum):
    """ Enum for specifying where an Op should run. """
    CPU = 0
    GPU = 1

    @staticmethod
    def to_proto(db, device):
        if device == DeviceType.CPU:
            return db.protobufs.CPU
        elif device == DeviceType.GPU:
            return db.protobufs.GPU
        else:
            raise ScannerException('Invalid device type')
