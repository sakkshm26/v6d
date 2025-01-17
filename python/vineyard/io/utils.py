#! /usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2020-2021 Alibaba Group Holding Limited.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import concurrent
import concurrent.futures
import json
import os
import traceback

from vineyard._C import ObjectID


def report_status(status, content):
    print(
        json.dumps(
            {
                'type': status,
                'content': content,
            }
        ),
        flush=True,
    )


def report_error(content):
    report_status('error', content)


def report_success(content):
    if isinstance(content, ObjectID):
        content = repr(content)
    report_status('return', content)


def report_exception():
    report_status('error', traceback.format_exc())


def expand_full_path(path):
    return os.path.expanduser(os.path.expandvars(path))


class BaseStreamExecutor:
    def execute(self):
        """ """


class ThreadStreamExecutor:
    def __init__(self, executor_cls, parallism: int = 1, **kwargs):
        self._parallism = parallism
        self._executors = [executor_cls(**kwargs) for _ in range(self._parallism)]

    def execute(self):
        def start_to_execute(executor: BaseStreamExecutor):
            return executor.execute()

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self._parallism
        ) as executor:
            results = [
                executor.submit(start_to_execute, exec) for exec in self._executors
            ]
            return [future.result() for future in results]
