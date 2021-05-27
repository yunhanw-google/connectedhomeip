/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

const listType = 'ARRAY';
const StructType = 'STRUCT';

function isList(type)
{
  return type.toUpperCase() == listType;
}

function isInt8U(type)
{
  return type == 'INT8U';
}

function isInt16U(type)
{
  return type == 'INT16U';
}

function isInt32U(type)
{
  return type == 'INT32U';
}

function isInt64U(type)
{
  return type == 'INT64U';
}

function isStruct(type)
{
  return type == StructType;
}
exports.isInt8U = isInt8U;
exports.isInt16U = isInt16U;
exports.isInt32U = isInt32U;
exports.isInt64U = isInt64U;
exports.isStruct = isStruct;
//
// Module exports
//
exports.isList = isList;
