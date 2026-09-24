#!/usr/bin/env python3

#
#    Copyright (c) 2026 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

# BLE Commissioning and Thread Provisioning test between MobileDevice and
# CHIPEndDevice.

import asyncio
import logging
import os
import sys

import base
from base import (
    BaseTestHelper,
    DEFAULT_REPL_STORAGE_PATH,
    FailIfNot,
    SetTestSet,
    TestFail,
    TestTimeout,
    logger,
)
import click
import coloredlogs
import matter.CertificateAuthority
from matter.ChipStack import ChipStack
import matter.clusters as Clusters
from matter.clusters.Types import NullValue
import matter.FabricAdmin
import matter.logging
import matter.native
from matter.storage import PersistentStorageJSON
from matter.tracing import TracingContext

TEST_THREAD_NETWORK_DATASET_TLV = (
    "0e080000000000010000"
    + "000300000c"
    + "35060004001fffe0"
    + "0208fedcba9876543210"
    + "0708fd00000000001234"
    + "0510ffeeddccbbaa99887766554433221100"
    + "030e54657374696e674e6574776f726b"
    + "0102d252"
    + "041081cb3b2efa781cc778397497ff520fa50c0302a0ff"
)
TEST_THREAD_NETWORK_ID = "fedcba9876543210"
TEST_DISCRIMINATOR = 3840
TEST_SETUPPIN = 20202021

ENDPOINT_ID = 0
LIGHTING_ENDPOINT_ID = 1

TEST_CONTROLLER_NODE_ID = 112233
TEST_DEVICE_NODE_ID = 1


class BleTestHelper(BaseTestHelper):

  def __init__(
      self,
      nodeId: int,
      paaTrustStorePath: str,
      bleAdapter: int = 0,
      testCommissioner: bool = False,
  ):
    matter.native.Init(bluetoothAdapter=bleAdapter)

    self.chipStack = ChipStack(
        PersistentStorageJSON(DEFAULT_REPL_STORAGE_PATH),
        enableServerInteractions=True,
    )
    self.certificateAuthorityManager = (
        matter.CertificateAuthority.CertificateAuthorityManager(
            chipStack=self.chipStack
        )
    )
    self.certificateAuthority = (
        self.certificateAuthorityManager.NewCertificateAuthority()
    )
    self.fabricAdmin = self.certificateAuthority.NewFabricAdmin(
        vendorId=0xFFF1, fabricId=1
    )
    self.devCtrl = self.fabricAdmin.NewController(
        nodeId, paaTrustStorePath, testCommissioner
    )
    self.controllerNodeId = nodeId
    self.logger = logger
    self.paaTrustStorePath = paaTrustStorePath
    logging.getLogger().setLevel(logging.DEBUG)


async def ble_thread_commissioning(
    test: BleTestHelper,
    discriminator: int,
    setup_pin: int,
    device_nodeid: int,
):
  logger.info(
      "Starting BLE Commissioning + Thread Provisioning (discriminator=%d,"
      " pin=%d, nodeId=%d)",
      discriminator,
      setup_pin,
      device_nodeid,
  )
  dataset_bytes = bytes.fromhex(TEST_THREAD_NETWORK_DATASET_TLV)
  commissioned_node_id = await test.devCtrl.CommissionBleThread(
      discriminator,
      setup_pin,
      device_nodeid,
      dataset_bytes,
  )
  FailIfNot(
      commissioned_node_id == device_nodeid,
      "CommissionBleThread failed or returned unexpected nodeId:"
      f" {commissioned_node_id}",
  )
  logger.info(
      "BLE Commissioning and Thread Provisioning succeeded for nodeId=%d",
      commissioned_node_id,
  )

  logger.info(
      "Verifying NetworkCommissioning cluster state provisioned over BLE"
  )
  res = await test.devCtrl.ReadAttribute(
      nodeId=device_nodeid,
      attributes=[
          (ENDPOINT_ID, Clusters.NetworkCommissioning.Attributes.Networks),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastNetworkID,
          ),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastNetworkingStatus,
          ),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastConnectErrorValue,
          ),
      ],
      returnClusterObject=True,
  )
  net_cluster = res[ENDPOINT_ID][Clusters.NetworkCommissioning]
  logger.info("Provisioned NetworkCommissioning attributes: %s", net_cluster)
  expected_net_id = bytes.fromhex(TEST_THREAD_NETWORK_ID)
  FailIfNot(
      len(net_cluster.networks) == 1,
      f"Expected 1 provisioned Thread network, got {len(net_cluster.networks)}",
  )
  FailIfNot(
      net_cluster.networks[0].networkID == expected_net_id,
      f"Expected networkID {expected_net_id.hex()}, got"
      f" {net_cluster.networks[0].networkID.hex()}",
  )
  FailIfNot(
      net_cluster.networks[0].connected,
      "Expected Thread network to be marked connected=True",
  )
  net_enums = Clusters.NetworkCommissioning.Enums
  k_success = net_enums.NetworkCommissioningStatusEnum.kSuccess
  FailIfNot(
      net_cluster.lastNetworkingStatus == k_success,
      "Expected LastNetworkingStatus=kSuccess, got"
      f" {net_cluster.lastNetworkingStatus}",
  )
  FailIfNot(
      net_cluster.lastNetworkID == expected_net_id,
      f"Expected LastNetworkID={expected_net_id.hex()}, got"
      f" {net_cluster.lastNetworkID}",
  )
  FailIfNot(
      net_cluster.lastConnectErrorValue == NullValue,
      "Expected LastConnectErrorValue=NullValue, got"
      f" {net_cluster.lastConnectErrorValue}",
  )


@base.test_case
def TestDatamodel(test: BleTestHelper, device_nodeid: int):
  logger.info("Testing datamodel functions over Thread/IPv6 operational CASE")

  logger.info("Testing on off cluster")
  FailIfNot(
      asyncio.run(
          test.TestOnOffCluster(
              nodeId=device_nodeid, endpoint=LIGHTING_ENDPOINT_ID
          )
      ),
      "Failed to test on off cluster",
  )

  logger.info("Testing level control cluster")
  FailIfNot(
      asyncio.run(
          test.TestLevelControlCluster(
              nodeId=device_nodeid, endpoint=LIGHTING_ENDPOINT_ID
          )
      ),
      "Failed to test level control cluster",
  )

  logger.info("Testing sending commands to non exist endpoint")
  FailIfNot(
      not asyncio.run(
          test.TestOnOffCluster(nodeId=device_nodeid, endpoint=233)
      ),
      "Failed to test on off cluster on non-exist endpoint",
  )

  logger.info("Testing attribute writing")
  FailIfNot(
      asyncio.run(
          test.TestWriteBasicAttributes(
              nodeId=device_nodeid, endpoint=ENDPOINT_ID
          )
      ),
      "Failed to test Write Basic Attributes",
  )

  logger.info("Testing attribute reading")
  FailIfNot(
      asyncio.run(
          test.TestReadBasicAttributes(
              nodeId=device_nodeid, endpoint=ENDPOINT_ID
          )
      ),
      "Failed to test Read Basic Attributes",
  )


def do_tests(
    controller_nodeid,
    device_nodeid,
    timeout,
    discriminator,
    setup_pin,
    ble_adapter,
    paa_trust_store_path,
):
  timeoutTicker = TestTimeout(timeout)
  timeoutTicker.start()

  test = BleTestHelper(
      nodeId=controller_nodeid,
      paaTrustStorePath=paa_trust_store_path,
      bleAdapter=ble_adapter,
  )

  asyncio.run(
      ble_thread_commissioning(test, discriminator, setup_pin, device_nodeid)
  )

  logger.info("Testing resolve")
  FailIfNot(test.TestResolve(nodeId=device_nodeid), "Failed to resolve nodeid")

  TestDatamodel(test, device_nodeid)

  logger.info("Testing closing sessions")
  FailIfNot(
      test.TestCloseSession(nodeId=device_nodeid), "Failed to close sessions"
  )

  timeoutTicker.stop()
  logger.info("BLE MobileDevice Test finished")
  sys.stdout.flush()
  sys.stderr.flush()
  os._exit(0)


@click.command()
@click.option(
    "--controller-nodeid",
    default=TEST_CONTROLLER_NODE_ID,
    type=int,
    help="NodeId of the controller.",
)
@click.option(
    "--device-nodeid",
    default=TEST_DEVICE_NODE_ID,
    type=int,
    help="NodeId of the device.",
)
@click.option(
    "--timeout",
    "-t",
    default=240,
    type=int,
    help="The program will return with timeout after specified seconds.",
)
@click.option(
    "--discriminator",
    default=TEST_DISCRIMINATOR,
    type=int,
    help="Discriminator of the device.",
)
@click.option(
    "--setup-pin",
    default=TEST_SETUPPIN,
    type=int,
    help="Setup pincode of the device.",
)
@click.option(
    "--ble-adapter",
    default=0,
    type=int,
    help="Bluetooth HCI adapter index (e.g. 0 for hci0).",
)
@click.option(
    "--enable-test",
    default=["all"],
    type=str,
    multiple=True,
    help="The tests to be executed.",
)
@click.option(
    "--disable-test",
    default=[],
    type=str,
    multiple=True,
    help="The tests to be excluded.",
)
@click.option(
    "--log-level",
    default="INFO",
    type=click.Choice(["ERROR", "WARN", "INFO", "DEBUG"]),
    help="The log level of the test.",
)
@click.option(
    "--log-format",
    default=None,
    type=str,
    help="Override logging format",
)
@click.option(
    "--paa-trust-store-path",
    default="",
    type=str,
    help="Path that contains valid and trusted PAA Root Certificates.",
)
@click.option(
    "--trace-to",
    multiple=True,
    default=[],
    help="Trace location",
)
def run(
    controller_nodeid,
    device_nodeid,
    timeout,
    discriminator,
    setup_pin,
    ble_adapter,
    enable_test,
    disable_test,
    log_level,
    log_format,
    paa_trust_store_path,
    trace_to,
):
  coloredlogs.install(level=log_level, fmt=log_format, logger=logger)
  SetTestSet(enable_test, disable_test)
  with TracingContext() as tracing_ctx:
    for destination in trace_to:
      tracing_ctx.StartFromString(destination)

    do_tests(
        controller_nodeid,
        device_nodeid,
        timeout,
        discriminator,
        setup_pin,
        ble_adapter,
        paa_trust_store_path,
    )


if __name__ == "__main__":
  try:
    run()
  except Exception as ex:
    logger.exception(ex)
    TestFail("Exception occurred when running BLE mobile device tests.")
