# ESP32-NUT-Server-USBHID
A demo running on ESP32-S3 to communicate with USB-HID UPS and to be a tiny NUT (Network UPS Tools) Server.

This is a simple project which contains a USB Host part to communicate with UPS as a HID Device, a protocol_examples_common to connect to specific Wi-Fi, a non-blocking TCP Server to response to work as a tiny NUT server, and a led_strip to control the RGB LED on board.

The UPS HID communication protocol is hard-code for [SANTAK TG-BOX 850](https://www.santak.com.cn/product/santak-tg-box-ups.html) (`driver.name: usbhid-ups; driver.version.data: MGE HID 1.32`). It may be uncapable with other UPS brands or models.

Some UPS data is hard-code, such as model, serial, vendorid, and so on.

The NUT UPS name is hard-code to be `qnapnas`. It will NOT check whether username or password is correct.

Do not use it in production environment, thus it has not been tested and may be stability issues.

## Video Demo
[![video_demo](README.assets/video-pic.png)](https://www.bilibili.com/video/BV1YP411p75y/)

or [https://www.bilibili.com/video/BV1YP411p75y/](https://www.bilibili.com/video/BV1YP411p75y/)

## Hardware Required
* Development board with USB capable ESP SoC and a USB-OTG feature USB Port (or you can DIY a USB Type-A female port with extra 5V power supply). I'm using [源地 ESP32-S3 N16R8 dev board](https://github.com/vcc-gnd/YD-ESP32-S3/).
* A USB cable for Power supply and programming.
* $1 USB OTG Cable in order to connect to UPS USB port.

**Notes for OTG 5V Power Supply:**

Some (or most) 3-rd party ESP32-S3 dev boards have a hidden pad (has a USB-OTG label) underneath the board. When it be shorted out, it can provide a 5V source to USB VBUS. more discussions at [here](https://github.com/touchgadget/esp32-usb-host-demos/issues/12).

Otherwise, you have to provide 5V to USB VBUS mannually, such as [DIY a USB cable](https://github.com/touchgadget/esp32-usb-host-demos) or DIY a USB Type-A female port with extra 5V power supply as below Pin assignments.

```
ESP BOARD    USB CONNECTOR (type A female)
                   --
                  | || VCC(Need extra 5V power support!)
[GPIO19]  ------> | || D-
[GPIO20]  ------> | || D+
                  | || GND
                   --
```

## Part: USB Host HID

### Pre-Analize

**Useful documents && tools**

- [pdcv11.pdf - Usage Tables for HID Power Devices 1.1](https://usb.org/document-library/usage-tables-hid-power-devices-11)
- [NUT_MGE_USB_Devices_Draft_AA.pdf](https://networkupstools.org/protocols/mge/NUT_MGE_USB_Devices_Draft_AA.pdf)
- [51029473zaac.pdf - Simplified SHUT](https://networkupstools.org/protocols/mge/51029473zaac.pdf)
- [Wireshark with USBPcap](https://www.wireshark.org/): To capture USB HID Descriptor and  packets.
- [Device Monitoring Studio](https://www.hhdsoftware.com/device-monitoring-studio): To [send HID packet](https://hhdsoftwaredocs.online/dms/advanced-features/usb-monitoring/hid-send.html).

My UPS (SANTAK TG-BOX 850) can be communicated with a QNAP NAS by a USB cable connected. It support a USB HID protocol for Power Device. I plugged it to my PC and got the HID Descriptor by using Wireshark.
```
05840904a1010910a1000912a100095a852075089501150026ff0065005500b182c00914a1000902a10206ffff09748503b183097481830975b18309758183c009e98507b183c0c009e085fe950a27ffffff7fb18209e085ff953fb1820584091ea1840943850d7510950127ffff00006621d15507b1830942750826ff006601f05500b183094085126721d1f0005507b182091f850b65005500b103c006ffff091ca100091ea18109988518751027ffff0000b183091f8517750826ff00b103c0091ea18209988518751027ffff0000b183091f8517750826ff00b103c0091ea18309988518751027ffff0000b183091f8517750826ff00b103c0091ea18409988518751027ffff0000b183091f8517750826ff00b103c0091ea18509988518751027ffff0000b183091f8517750826ff00b103c0c005840918a1000920a181091f850bb1030921b1030902a102096c850cb103096b8502b183096b8183c006ffff09e98507b183c005840920a182091f850bb1030921b1030902a102096c8519b103096b8502b183096b8183c006ffff09e98507b183c005840919850bb103c00916a10006ffff0941850cb1030914a10005840902a10209628503b18309628183c0c0091ca10009548513751027ffff00006721d1f0005507b18209538514750826ff00b182091d850b65005500b1030902a10206ffff094a8503b183094a8183c005840930850e751027ffff00006721d1f0005507b183c00917850b750826ff0065005500b103c00924a10006ffff0922a10009988507b183c00584095a851fb1820585098d850cb103092cb10306ffff09958515b103058409578509752015ff27ffffff7f660110b1820956850ab18205850983850c7508150026ff006500b1030584091f850bb10305850967850cb10309898510b103058409fdb10306ffff09f0b103058409feb10306ffff09f4b103058409ffb10306ffff09f1b10309948507b18305840935b1830925850bb1030902a102058509d0850175012501b18309d081830942b183094281830944b1830944818305840973b1830973818305850945b1830945818305840961b183096181830962b183096281830585094bb183094b818305840965750826ff00b183096581830969b18309698183c0058509668506b1830966818309298508b18306ffff094d8522b182058509688506752027ffffff7f660110b1830968818306ffff09e98507750826ff006500b18309e2850cb103c009e5a1000905a10009f18510b103099485fdb182c0c0c0
```
The USB Host(such as PC or NAS) will send GET_REPORT packet(`bRequest=GET_REPORT or 0x01, ReportType = Feature or 3`) timely to UPS, and the UPS will return a bytes buffer contains battery and ups info.
The returned first byte will always be the Report ID.

Report ID: 0x01 (brief information):
```
- byte[0]: 0x01 - as Report ID
- byte[1]: 0  0  1  0  0  1  0  1
           |NeedReplacement: No
              |InternalFailure: No
                 |Good: Yes
                    |Discharging: No
                       |CommunicationLost: No
                          |Charging: Yes
                             |BelowRemainingCapacityLimit: No
                                |ACPresent: Yes
- byte[2]: 0x00 - Overload Flag: will gt than 0x00 if overload
- byte[3]: 0x00 - ShutdownImminent Flag: will gt than 0x00 if AC not present and remaining capacity lt remaining capacity limit (20%)
```

Report ID: 0x06 (battery remaining capacity (in percent) and remaining runtime (in sec)):
```
- byte[0]: 0x06 - as Report ID
- byte[1]: 0x64 - battery remaining capacity is 100%
- byte[2]: 0xe0
- byte[3]: 0x04
- byte[4]: 0x00
- byte[5]: 0x00 - <00 00 04 e0> as 1248 seconds left for runtime.
```

Report ID: 0x07 (some IDs and current load (in percent)):
```
- byte[0]: 0x07 - as Report ID
- byte[1]: 0x02 - other things
- byte[2]: 0x02 - other things
- byte[3]: 0x01 - other things
- byte[4]: 0x00 - other things
- byte[5]: 0x03 - other things
- byte[6]: 0x18 - current load is 24%
- byte[7]: 0x02 - other things
```

Report ID: 0x08 (remaining capacity limit):
```
- byte[0]: 0x08 - as Report ID
- byte[1]: 0x14 - remaining capacity limit is 20%
```

Report ID: 0x0d (config apparent power and config frequency):
```
- byte[0]: 0x0d - as Report ID
- byte[1]: 0x52
- byte[2]: 0x03 - <03 52> as config apparent power is 850W
- byte[3]: 0x32 - 50Hz as config frequency
```

Report ID: 0x0e (actual voltage):
```
- byte[0]: 0x0e - as Report ID
- byte[1]: 0xe6
- byte[2]: 0x00 - <00 e6> as actual voltage is 230V
```

Report ID: 0x12 (config voltage):
```
- byte[0]: 0x12 - as Report ID
- byte[1]: 0xdc - as config voltage is 220V
```

Report ID: 0x13 (allow highest voltage):
```
- byte[0]: 0x13 - as Report ID
- byte[1]: 0x08
- byte[2]: 0x01 - <01 08> is 264V
```

Report ID: 0x14 (allow lowest voltage):
```
- byte[0]: 0x14 - as Report ID
- byte[1]: 0xb8 - is 184V.
```

Thus the UPS support 184~264V input, and 220V is standard input. Real input voltage is 230V at this time.

Report ID: 0x1f (get (or set) beep sound):
```
- byte[0]: 0x1f - as Report ID
- byte[1]: 0x02 - beep sound is enabled. 01 is disabled (never sound), and 03 is muted (temporarily silence the alarm). See more by searching AudibleAlarmControl.
```

You can also use SET_REPORT method to set beep sound. data fragment as `0x1f 0x01` is to disable it.

### Init USB Host and so on
You can follow the [USB HID Class example](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/hid) provided by ESP official. Remember to use the master branch or any future release gt v5.1. The example in v5.1 does not contain `hid_class_request_get_report`, and the whole structure is different.

I export the `hid_host_device_handle_t` (as `latest_hid_device_handle`) object in order to use `hid_class_request_get_report` more easily. (The best practice *may* need to gen a new handle for UPS?)

### UPS plug in & out
When it plug in, `hid_host_device_event` will be triggered. When it plug out, `hid_host_interface_callback` will be triggered. So the `UPS_DEV_CONNECTED` falg is changed there. And my UPS hid protocol is none.

### Communicat with UPS
a per-1-second timer is set to run `refresh_ups_status_from_hid` function if UPS is connected. In that function, `hid_class_request_get_report` will be used to gen specific protocol data according to the pre-analize and send to UPS, then get the report contained needed information. It will also update the cJSON object which should contains the latest UPS info and will be sent to NUT clients in TCP-Server part.

`set_beep` function will use `hid_class_request_set_report` to change the beep status of UPS.

## Part: General Purpose Timer
A gptimer is set to trigger at each second. When triggered, it will call `timer_on_alarm_callback`. As this function can not run any blocking functions, so it push an element to `timer_queue` (also `user_ctx` para). The running task `timer_task` created by `xTaskCreate` can receive this element and do actual things in it, such as `refresh_ups_status_from_hid`, change beep status if BOOT_BUTTON is pressed and so on.

## Part: LED Strip
A simple way to show some status. The LED shows green if UPS connected, shows orange if UPS disconnected, shows red if UPS has error or AC not present, and shows white if a NUT client ask for data. Search `led_strip_set_pixel` in code file.

## Part: Non-blocking TCP Server
This part is as a very simple NUT Server. It can only respond `USERNAME` `PASSWORD` `LOGIN` `LIST VAR` `GET VAR qnapups ups.status` and `LOGOUT` (in `tcp_server_task` func). It seems that a NUT client will only ask for these information on a regular basis.

The UPS info is stored as cJSON object and will be updated at each `refresh_ups_status_from_hid` called. When a NUT client ask for info, it will print cJSON in a specific format (by calling `gen_nut_list_var_text_wrapper`) and return to client.