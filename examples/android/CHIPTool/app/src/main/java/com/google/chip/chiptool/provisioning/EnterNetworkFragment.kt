/*
 *   Copyright (c) 2020 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
package com.google.chip.chiptool.provisioning

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.Toast
import androidx.fragment.app.Fragment
import com.google.chip.chiptool.NetworkCredentialsParcelable
import com.google.chip.chiptool.R
import com.google.chip.chiptool.util.FragmentUtil

import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Proxy

/**
 * Fragment to collect Wi-Fi network information from user and send it to device being provisioned.
 */
class EnterNetworkFragment : Fragment() {
  private val networkType: ProvisionNetworkType
    get() =
      requireNotNull(
        ProvisionNetworkType.fromName(arguments?.getString(ARG_PROVISION_NETWORK_TYPE))
      )

  interface Callback {
    fun onNetworkCredentialsEntered(networkCredentials: NetworkCredentialsParcelable)
  }

  override fun onCreateView(
    inflater: LayoutInflater,
    container: ViewGroup?,
    savedInstanceState: Bundle?
  ): View? {
    val layoutRes =
      when (networkType) {
        ProvisionNetworkType.WIFI -> R.layout.enter_wifi_network_fragment
        ProvisionNetworkType.THREAD -> R.layout.enter_thread_network_fragment
      }

    return inflater.inflate(layoutRes, container, false).apply {
      val saveNetworkBtn: Button = findViewById(R.id.saveNetworkBtn)
      saveNetworkBtn.setOnClickListener { onSaveNetworkClicked(this) }
      if (networkType == ProvisionNetworkType.THREAD) {
        findViewById<Button>(R.id.fetchThreadDatasetBtn)?.setOnClickListener {
          fetchThreadDatasetFromSystem(this)
        }
      }
    }
  }

  private fun onSaveNetworkClicked(view: View) {
    if (networkType == ProvisionNetworkType.WIFI) {
      saveWiFiNetwork(view)
    } else {
      saveThreadNetwork(view)
    }
  }

  private fun saveWiFiNetwork(view: View) {
    val ssidEd: EditText = view.findViewById(R.id.ssidEd)
    val pwdEd: EditText = view.findViewById(R.id.pwdEd)
    val ssid = ssidEd?.text
    val pwd = pwdEd?.text

    if (ssid.isNullOrBlank() || pwd.isNullOrBlank()) {
      Toast.makeText(requireContext(), "Ssid and password required.", Toast.LENGTH_SHORT).show()
      return
    }

    val networkCredentials =
      NetworkCredentialsParcelable.forWiFi(
        NetworkCredentialsParcelable.WiFiCredentials(ssid.toString(), pwd.toString())
      )
    FragmentUtil.getHost(this, Callback::class.java)
      ?.onNetworkCredentialsEntered(networkCredentials)
  }

  private fun saveThreadNetwork(view: View) {
    val channelEd: EditText = view.findViewById(R.id.channelEd)
    val panIdEd: EditText = view.findViewById(R.id.panIdEd)
    val xpanIdEd: EditText = view.findViewById(R.id.xpanIdEd)
    val masterKeyEd: EditText = view.findViewById(R.id.masterKeyEd)
    val channelStr = channelEd.text
    val panIdStr = panIdEd.text

    if (channelStr.isNullOrBlank()) {
      Toast.makeText(requireContext(), "Channel is empty", Toast.LENGTH_SHORT).show()
      return
    }

    if (panIdStr.isNullOrBlank()) {
      Toast.makeText(requireContext(), "PAN ID is empty", Toast.LENGTH_SHORT).show()
      return
    }

    if (xpanIdEd.text.isNullOrBlank()) {
      Toast.makeText(requireContext(), "XPAN ID is empty", Toast.LENGTH_SHORT).show()
      return
    }

    val xpanIdStr = xpanIdEd.text.toString().filterNot { c -> c == ':' }
    if (xpanIdStr.length != NUM_XPANID_BYTES * 2) {
      Toast.makeText(requireContext(), "Extended PAN ID is invalid", Toast.LENGTH_SHORT).show()
      return
    }

    if (masterKeyEd.text.isNullOrBlank()) {
      Toast.makeText(requireContext(), "Master Key is empty", Toast.LENGTH_SHORT).show()
      return
    }

    val masterKeyStr = masterKeyEd.text.toString().filterNot { c -> c == ':' }
    if (masterKeyStr.length != NUM_MASTER_KEY_BYTES * 2) {
      Toast.makeText(requireContext(), "Master key is invalid", Toast.LENGTH_SHORT).show()
      return
    }

    val operationalDataset =
      makeThreadOperationalDataset(
        channelStr.toString().toInt(),
        panIdStr.toString().toInt(16),
        xpanIdStr.hexToByteArray(),
        masterKeyStr.hexToByteArray()
      )

    val networkCredentials =
      NetworkCredentialsParcelable.forThread(
        NetworkCredentialsParcelable.ThreadCredentials(operationalDataset)
      )
    FragmentUtil.getHost(this, Callback::class.java)
      ?.onNetworkCredentialsEntered(networkCredentials)
  }

  private fun makeThreadOperationalDataset(
    channel: Int,
    panId: Int,
    xpanId: ByteArray,
    masterKey: ByteArray
  ): ByteArray {
    // channel
    var dataset = byteArrayOf(TYPE_CHANNEL.toByte(), NUM_CHANNEL_BYTES.toByte())
    dataset += 0x00.toByte() // Channel Page 0.
    dataset += (channel.shr(8) and 0xFF).toByte()
    dataset += (channel and 0xFF).toByte()

    // PAN ID
    dataset += TYPE_PANID.toByte()
    dataset += NUM_PANID_BYTES.toByte()
    dataset += (panId.shr(8) and 0xFF).toByte()
    dataset += (panId and 0xFF).toByte()

    // Extended PAN ID
    dataset += TYPE_XPANID.toByte()
    dataset += NUM_XPANID_BYTES.toByte()
    dataset += xpanId

    // Network Master Key
    dataset += TYPE_MASTER_KEY.toByte()
    dataset += NUM_MASTER_KEY_BYTES.toByte()
    dataset += masterKey

    return dataset
  }

  private fun String.hexToByteArray(): ByteArray {
    return chunked(2).map { byteStr -> byteStr.toUByte(16).toByte() }.toByteArray()
  }

  private fun fetchThreadDatasetFromSystem(view: View) {
    val context = requireContext()

    // Pass 1: Try GMS ThreadNetwork API (allCredentials & getPreferredCredentials)
    try {
      Log.d(TAG, "Attempting GMS ThreadNetwork.getClient().allCredentials")
      com.google.android.gms.threadnetwork.ThreadNetwork.getClient(context)
        .allCredentials
        .addOnSuccessListener { credentialsList ->
          Log.d(TAG, "GMS getAllCredentials onSuccess: size=${credentialsList?.size}")
          if (!credentialsList.isNullOrEmpty()) {
            val credentials = credentialsList[0]
            val tlvs = credentials.activeOperationalDataset
            if (tlvs != null && tlvs.isNotEmpty()) {
              Log.d(TAG, "Successfully fetched Thread dataset TLVs from GMS getAllCredentials()")
              requireActivity().runOnUiThread {
                populateThreadFieldsFromTlvs(view, tlvs)
                Toast.makeText(
                  context,
                  R.string.enter_thread_fetch_success,
                  Toast.LENGTH_SHORT
                ).show()
              }
              return@addOnSuccessListener
            }
          }
          Log.w(TAG, "GMS ThreadNetwork returned empty credentials list, attempting getPreferredCredentials consent fallback")
          fetchPreferredGmsCredentials(view)
        }
        .addOnFailureListener { e ->
          Log.w(TAG, "GMS ThreadNetwork getAllCredentials failed, attempting getPreferredCredentials consent fallback", e)
          fetchPreferredGmsCredentials(view)
        }
      return
    } catch (e: NoClassDefFoundError) {
      Log.d(TAG, "GMS ThreadNetwork API class not found, falling back to system service", e)
    } catch (e: Exception) {
      Log.d(TAG, "GMS ThreadNetwork API failed, falling back to system service", e)
    }

    fetchThreadDatasetFromSystemService(view)
  }

  private fun fetchPreferredGmsCredentials(view: View) {
    val context = requireContext()
    try {
      com.google.android.gms.threadnetwork.ThreadNetwork.getClient(context)
        .preferredCredentials
        .addOnSuccessListener { result ->
          Log.d(TAG, "GMS getPreferredCredentials onSuccess: $result")
          if (result != null && result.intentSender != null) {
            try {
              startIntentSenderForResult(
                result.intentSender,
                REQUEST_CODE_THREAD_GMS_CONSENT,
                null, 0, 0, 0, null
              )
              return@addOnSuccessListener
            } catch (e: Exception) {
              Log.e(TAG, "Failed to launch GMS Thread consent intent", e)
            }
          }
          fetchThreadDatasetFromSystemService(view)
        }
        .addOnFailureListener { e ->
          Log.w(TAG, "GMS getPreferredCredentials failed", e)
          fetchThreadDatasetFromSystemService(view)
        }
    } catch (e: Exception) {
      Log.e(TAG, "Error calling getPreferredCredentials", e)
      fetchThreadDatasetFromSystemService(view)
    }
  }

  override fun onActivityResult(requestCode: Int, resultCode: Int, data: android.content.Intent?) {
    super.onActivityResult(requestCode, resultCode, data)
    if (requestCode == REQUEST_CODE_THREAD_GMS_CONSENT) {
      if (resultCode == android.app.Activity.RESULT_OK && data != null) {
        try {
          val credentials = com.google.android.gms.threadnetwork.ThreadNetworkCredentials.fromIntentSenderResultData(data)
          val tlvs = credentials.activeOperationalDataset
          if (tlvs != null && tlvs.isNotEmpty()) {
            val view = view ?: return
            populateThreadFieldsFromTlvs(view, tlvs)
            Toast.makeText(requireContext(), R.string.enter_thread_fetch_success, Toast.LENGTH_SHORT).show()
          }
        } catch (e: Exception) {
          Log.e(TAG, "Failed to parse ThreadNetworkCredentials from intent result", e)
        }
      }
    }
  }

  private fun fetchThreadDatasetFromSystemService(view: View) {
    val context = requireContext()
    if (Build.VERSION.SDK_INT < 34) {
      Toast.makeText(
        context,
        "ThreadNetworkManager requires Android 14 (API 34) or higher",
        Toast.LENGTH_SHORT
      ).show()
      return
    }

    val threadNetworkManager = try {
      context.getSystemService("thread_network")
    } catch (e: Exception) {
      Log.e(TAG, "Failed to access ThreadNetworkManager system service", e)
      null
    }

    if (threadNetworkManager == null) {
      Toast.makeText(
        context,
        R.string.enter_thread_manager_unavailable,
        Toast.LENGTH_SHORT
      ).show()
      return
    }

    try {
      val getControllersMethod = threadNetworkManager.javaClass.getMethod("getAllThreadNetworkControllers")
      @Suppress("UNCHECKED_CAST")
      val controllers = getControllersMethod.invoke(threadNetworkManager) as? List<*>
      if (controllers.isNullOrEmpty()) {
        Toast.makeText(
          context,
          "No ThreadNetworkController available on this device",
          Toast.LENGTH_SHORT
        ).show()
        return
      }

      val controller = controllers[0]!!
      val executor = ContextCompat.getMainExecutor(context)

      val callbackClass = Class.forName("android.net.thread.ThreadNetworkController\$OperationalDatasetCallback")
      var proxyCallback: Any? = null
      val invocationHandler = InvocationHandler { proxy, method, args ->
        when (method.name) {
          "hashCode" -> return@InvocationHandler System.identityHashCode(proxy)
          "equals" -> return@InvocationHandler (proxy === args?.get(0))
          "toString" -> return@InvocationHandler "OperationalDatasetCallbackProxy@${Integer.toHexString(System.identityHashCode(proxy))}"
          "onActiveOperationalDatasetChanged" -> {
            if (args != null && args.isNotEmpty()) {
              val activeDataset = args[0]
              if (activeDataset != null) {
                try {
                  val toTlvsMethod = activeDataset.javaClass.getMethod("toThreadTlvs")
                  val tlvs = toTlvsMethod.invoke(activeDataset) as ByteArray
                  requireActivity().runOnUiThread {
                    populateThreadFieldsFromTlvs(view, tlvs)
                    Toast.makeText(
                      context,
                      R.string.enter_thread_fetch_success,
                      Toast.LENGTH_SHORT
                    ).show()
                  }
                } catch (e: Exception) {
                  Log.e(TAG, "Failed to parse active operational dataset TLVs", e)
                }
              } else {
                requireActivity().runOnUiThread {
                  Toast.makeText(
                    context,
                    "No active Thread operational dataset found on system",
                    Toast.LENGTH_SHORT
                  ).show()
                }
              }
              try {
                val unregisterMethod = controller.javaClass.getMethod(
                  "unregisterOperationalDatasetCallback",
                  callbackClass
                )
                proxyCallback?.let { unregisterMethod.invoke(controller, it) }
              } catch (e: Exception) {
                Log.e(TAG, "Failed to unregister callback", e)
              }
            }
          }
        }
        null
      }

      proxyCallback = Proxy.newProxyInstance(
        callbackClass.classLoader,
        arrayOf(callbackClass),
        invocationHandler
      )

      val registerMethod = controller.javaClass.getMethod(
        "registerOperationalDatasetCallback",
        java.util.concurrent.Executor::class.java,
        callbackClass
      )
      registerMethod.invoke(controller, executor, proxyCallback)

    } catch (e: Exception) {
      Log.e(TAG, "Error querying ThreadNetworkManager", e)
      Toast.makeText(
        context,
        getString(R.string.enter_thread_fetch_error, e.message ?: "Unknown error"),
        Toast.LENGTH_SHORT
      ).show()
    }
  }

  private fun populateThreadFieldsFromTlvs(view: View, tlvs: ByteArray) {
    var channel: Int? = null
    var panId: Int? = null
    var xpanId: ByteArray? = null
    var masterKey: ByteArray? = null

    var i = 0
    while (i + 1 < tlvs.size) {
      val type = tlvs[i].toInt() and 0xFF
      val length = tlvs[i + 1].toInt() and 0xFF
      if (i + 2 + length > tlvs.size) {
        break
      }
      val value = tlvs.copyOfRange(i + 2, i + 2 + length)
      when (type) {
        TYPE_CHANNEL -> {
          if (value.size >= 3) {
            channel = ((value[1].toInt() and 0xFF) shl 8) or (value[2].toInt() and 0xFF)
          }
        }
        TYPE_PANID -> {
          if (value.size >= 2) {
            panId = ((value[0].toInt() and 0xFF) shl 8) or (value[1].toInt() and 0xFF)
          }
        }
        TYPE_XPANID -> {
          if (value.size == NUM_XPANID_BYTES) {
            xpanId = value
          }
        }
        TYPE_MASTER_KEY -> {
          if (value.size == NUM_MASTER_KEY_BYTES) {
            masterKey = value
          }
        }
      }
      i += 2 + length
    }

    channel?.let {
      view.findViewById<EditText>(R.id.channelEd).setText(it.toString())
    }
    panId?.let {
      val hexPanId = String.format("%04X", it)
      view.findViewById<EditText>(R.id.panIdEd).setText(hexPanId)
    }
    xpanId?.let {
      val hexXpanId = it.toFormattedHexString()
      view.findViewById<EditText>(R.id.xpanIdEd).setText(hexXpanId)
    }
    masterKey?.let {
      val hexMasterKey = it.toFormattedHexString()
      view.findViewById<EditText>(R.id.masterKeyEd).setText(hexMasterKey)
    }
  }

  private fun ByteArray.toFormattedHexString(): String {
    return joinToString(":") { "%02X".format(it) }
  }

  companion object {
    private const val TAG = "EnterNetworkFragment"
    private const val REQUEST_CODE_THREAD_GMS_CONSENT = 1002
    private const val ARG_PROVISION_NETWORK_TYPE = "provision_network_type"
    private const val NETWORK_COMMISSIONING_CLUSTER_ENDPOINT = 0

    private const val NUM_CHANNEL_BYTES = 3
    private const val NUM_PANID_BYTES = 2
    private const val NUM_XPANID_BYTES = 8
    private const val NUM_MASTER_KEY_BYTES = 16
    private const val TYPE_CHANNEL = 0 // Type of Thread Channel TLV.
    private const val TYPE_PANID = 1 // Type of Thread PAN ID TLV.
    private const val TYPE_XPANID = 2 // Type of Thread Extended PAN ID TLV.
    private const val TYPE_MASTER_KEY = 5 // Type of Thread Network Master Key TLV.

    fun newInstance(provisionNetworkType: ProvisionNetworkType): EnterNetworkFragment {
      return EnterNetworkFragment().apply {
        arguments =
          Bundle(1).apply { putString(ARG_PROVISION_NETWORK_TYPE, provisionNetworkType.name) }
      }
    }
  }
}
