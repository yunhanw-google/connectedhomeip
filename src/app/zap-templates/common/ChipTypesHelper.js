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

const zapPath = '../../../../third_party/zap/repo/dist/src-electron/'
const ListHelper = require('./ListHelper.js')
const StringHelper = require('./StringHelper.js')
const StructHelper = require('./StructHelper.js')
const zclHelper = require(zapPath + 'generator/helper-zcl.js')

function asBasicType(type) {
    switch (type) {
        case 'chip::ActionId':
        case 'chip::FabricIndex':
            return 'uint8_t'
        case 'chip::EndpointId':
        case 'chip::GroupId':
        case 'chip::VendorId':
            return 'uint16_t'
        case 'chip::ClusterId':
        case 'chip::AttributeId':
        case 'chip::FieldId':
        case 'chip::EventId':
        case 'chip::CommandId':
        case 'chip::TransactionId':
        case 'chip::DeviceTypeId':
        case 'chip::StatusCode':
        case 'chip::DataVersion':
            return 'uint32_t'
        case 'chip::EventNumber':
        case 'chip::FabricId':
        case 'chip::NodeId':
            return 'uint64_t'
        default:
            return type
    }
}

function asNotSupportedBasicType(type) {
    return asChipZapType(type) == 'Unsupported';
}

function asChipZapType(type) {
    if (StringHelper.isOctetString(type)) {
        return 'chip::ByteSpan'
    }

    if (StringHelper.isCharString(type)) {
        return 'Span<const char>'
    }

    // Need to return actual enum type
    if (isEnum(type)) {
        return type
    }

    switch (type) {
        case 'BOOLEAN':
            return 'bool'
        case 'INT8S':
            return  'int8_t'
        case 'INT16S':
            return  'int16_t'
        case 'INT24S':
            return  'int24_t'
        case 'INT32S':
            return  'int32_t'
        case 'INT64S':
            return  'int64_t'
        case 'INT8U':
            return  'uint8_t'
        case 'INT16U':
            return  'uint16_t'
        case 'INT24U':
            return  'uint24_t'
        case 'INT32U':
            return  'uint32_t'
        case 'INT64U':
            return  'uint64_t'
        default:
            return 'Unsupported'
    }
}

function isEnum(type)
{
    return type.toUpperCase() == 'ENUM8' || type.toUpperCase() == 'ENUM16'
}

//
// Module exports
//
exports.asBasicType = asBasicType
exports.asNotSupportedBasicType = asNotSupportedBasicType
exports.asChipZapType = asChipZapType
exports.isEnum = isEnum;
