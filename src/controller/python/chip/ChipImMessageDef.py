#!/usr/bin/env python3
# coding=utf-8

#
#   Copyright (c) 2021 Project CHIP Authors
#   All rights reserved.
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#

#
#   @file
#         This file contains python definitions for working with data encoded in Chip InteractionModel Message
#


from __future__ import absolute_import
from __future__ import print_function

import struct
from collections import Mapping, Sequence, OrderedDict

import ChipTLV


class StructureBuildBase(object):
    def __init__(self):
        self._fields = {}

    def encode(self):
        tlvWriter = ChipTLV.TLVWriter()
        tlvWriter.put(None, self._fields)

        return tlvWriter.encoding

    def __str__(self):
        return str(self.encode())

    def get_fields(self):
        return self._fields

    def _setField(self, tag, value):
        if value != None:
            self._fields[tag] = value
        else:
            self._fields.pop(tag, None)


class ListBuildBase(object):
    def __init__(self):
        self._fields = []

    def encode(self):
        tlvWriter = ChipTLV.TLVWriter()
        tlvWriter.put(None, self._fields)
        return tlvWriter.encoding

    def __str__(self):
        return str(self.encode())

    def get_fields(self):
        return self._fields


class CommandPath(StructureBuildBase):
    _tag_endpoint_id = 0
    _tag_group_id = 1
    _tag_cluster_id = 2
    _tag_command_id = 3

    @property
    def endpoint_id(self):
        return self._fields.get(CommandPath._tag_endpoint_id, None)

    @endpoint_id.setter
    def endpoint_id(self, value):
        self._setField(CommandPath._tag_endpoint_id, value)

    @property
    def group_id(self):
        return self._fields.get(CommandPath._tag_group_id, None)

    @group_id.setter
    def group_id(self, value):
        self._setField(CommandPath._tag_group_id, value)

    @property
    def cluster_id(self):
        return self._fields.get(CommandPath._tag_cluster_id, None)

    @cluster_id.setter
    def cluster_id(self, value):
        self._setField(CommandPath._tag_cluster_id, value)

    @property
    def command_id(self):
        return self._fields.get(CommandPath._tag_command_id, None)

    @command_id.setter
    def command_id(self, value):
        self._setField(CommandPath._tag_command_id, value)


class StatusElement(StructureBuildBase):
    _tag_general_code = 0
    _tag_protocol_id = 1
    _tag_protocol_code = 2
    _tag_cluster_id = 3

    @property
    def general_code(self):
        return self._fields.get(StatusElement._tag_general_code, None)

    @general_code.setter
    def general_code(self, value):
        self._setField(StatusElement._tag_general_code, value)

    @property
    def protocol_id(self):
        return self._fields.get(StatusElement._tag_protocol_id, None)

    @protocol_id.setter
    def protocol_id(self, value):
        self._setField(StatusElement._tag_protocol_id, value)

    @property
    def protocol_code(self):
        return self._fields.get(StatusElement._tag_protocol_code, None)

    @protocol_code.setter
    def protocol_code(self, value):
        self._setField(StatusElement._tag_protocol_code, value)

    @property
    def cluster_id(self):
        return self._fields.get(StatusElement._tag_cluster_id, None)

    @cluster_id.setter
    def cluster_id(self, value):
        self._setField(StatusElement._tag_cluster_id, value)


class CommandDataElement(StructureBuildBase):
    _tag_command_path = 0
    _tag_data = 1
    _tag_status_element = 2

    @property
    def command_path(self):
        return self._fields.get(CommandDataElement._tag_command_path, None)

    @command_path.setter
    def command_path(self, value):
        self._setField(CommandDataElement._tag_command_path, value)

    @property
    def data(self):
        return self._fields.get(CommandDataElement._tag_data, None)

    @data.setter
    def data(self, value):
        self._setField(CommandDataElement._tag_data, value)

    @property
    def status_element(self):
        return self._fields.get(CommandDataElement._tag_status_element, None)

    @status_element.setter
    def status_element(self, value):
        self._setField(CommandDataElement._tag_status_element, value)


class CommandList(ListBuildBase):
    def __init__(self):
        self._fields = []

    def add_command_data_element(self, value):
        self._fields.append(value)

    def encode(self):
        tlvWriter = ChipTLV.TLVWriter()
        tlvWriter.put(None, self._fields)
        return tlvWriter.encoding

    def __str__(self):
        return str(self.encode())

    def get_fields(self):
        return self._fields


class CommandMessage(StructureBuildBase):
    _tag_command_list = 0

    @property
    def command_list(self):
        return self._fields.get(CommandMessage._tag_command_list, None)

    @command_list.setter
    def command_list(self, value):
        self._setField(CommandMessage._tag_command_list, value)


def validate_invoke_command_request():
    # Invoke Command Request
    invoke_command_request = CommandMessage()

    command_path = CommandPath()
    command_path.endpoint_id = 0x1234
    command_path.group_id = 20
    command_path.cluster_id = 0xDDAA
    command_path.command_id = 40

    commandDataElement = CommandDataElement()
    commandDataElement.command_path = command_path.get_fields()
    # Fill in Command Argument, tag with value
    commandDataElement.data = {1: 2}

    command_list = CommandList()
    command_list.add_command_data_element(commandDataElement.get_fields())

    invoke_command_request.command_list = command_list.get_fields()
    # Invoke Command Response

    reader = ChipTLV.TLVReader(invoke_command_request.encode())
    out = reader.get()

    print("Invoke Command Request message Test result is as below !!!!!!")
    print("input: " + str(invoke_command_request.get_fields()))
    print("TLV encoded result: " + str(invoke_command_request.encode()))
    print("TLV reader's output: " + str(out["Any"]))

    if invoke_command_request.get_fields() == out["Any"]:
        print("Invoke Command Request message Test Success")
    else:
        print("Invoke Command Request message Test Failure")


def validate_invoke_command_response():
    # Invoke Command Request
    invoke_command_response = CommandMessage()

    command_path = CommandPath()
    command_path.endpoint_id = 0x1234
    command_path.group_id = 20
    command_path.cluster_id = 0xDDAA
    command_path.command_id = 41

    commandDataElement = CommandDataElement()
    commandDataElement.command_path = command_path.get_fields()
    # Fill in Command Argument, tag with value
    commandDataElement.data = {1: 2}

    command_list = CommandList()
    command_list.add_command_data_element(commandDataElement.get_fields())

    invoke_command_response.command_list = command_list.get_fields()
    # Invoke Command Response

    reader = ChipTLV.TLVReader(invoke_command_response.encode())
    out = reader.get()

    print("Invoke Command Response message Test result is as below !!!!!!")
    print("input: " + str(invoke_command_response.get_fields()))
    print("TLV encoded result: " + str(invoke_command_response.encode()))
    print("TLV reader's output: " + str(out["Any"]))

    if invoke_command_response.get_fields() == out["Any"]:
        print("Invoke Command Response message Test Success")
    else:
        print("Invoke Command Response message Test Failure")


if __name__ == "__main__":
    validate_invoke_command_request()
    validate_invoke_command_response()
