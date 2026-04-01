########################################################################
# Android NDK build: librtl433_ffi.so
#
# Compiles rtl_433 core + librtlsdr + libusb + FFI wrapper as a single
# shared library.  librtlsdr and libusb are taken from the rtl_tcp_andro
# tree which includes Android-specific patches (rtlsdr_open2 / libusb_open2).
#
# Used by the Flutter app via externalNativeBuild in build.gradle.kts.
########################################################################

cmake_minimum_required(VERSION 3.18)
project(rtl433_ffi C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)

# Silence warnings in third-party code
add_compile_options(-Wno-unused-parameter -Wno-sign-compare -Wno-pedantic)
add_definitions(-D_GNU_SOURCE)
add_definitions(-DTHREADS)
add_definitions(-DMG_ENABLE_IPV6=1)
add_definitions(-DANDROID)
add_definitions(-DRTLSDR)

# Suppress verbose "internals changed" notice from rtl_433 startup
add_definitions(-DNDEBUG)

# Path to the rtl_433 source tree (this CMakeLists lives in rtl_433/android/)
set(RTL433_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(RTL433_INC "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Paths to the Android-patched librtlsdr and libusb inside rtl_tcp_andro
set(ANDRO_CPP   "${CMAKE_CURRENT_SOURCE_DIR}/../../rtl_tcp_andro-/rtlsdr/src/main/cpp")
set(RTLSDR_SRC  "${ANDRO_CPP}/librtlsdr/src")
set(RTLSDR_INC  "${ANDRO_CPP}/librtlsdr/include")
set(LIBUSB_ROOT "${ANDRO_CPP}/librtlsdr/libusb")
set(LIBUSB_SRC  "${LIBUSB_ROOT}/libusb")

# libusb sources (Android / linux_usbfs backend)
set(LIBUSB_SOURCES
    ${LIBUSB_SRC}/core.c
    ${LIBUSB_SRC}/descriptor.c
    ${LIBUSB_SRC}/hotplug.c
    ${LIBUSB_SRC}/io.c
    ${LIBUSB_SRC}/strerror.c
    ${LIBUSB_SRC}/sync.c
    ${LIBUSB_SRC}/os/linux_usbfs.c
    ${LIBUSB_SRC}/os/linux_netlink.c
    ${LIBUSB_SRC}/os/poll_posix.c
    ${LIBUSB_SRC}/os/threads_posix.c
)

# librtlsdr sources (Android-patched, includes rtlsdr_open2)
set(RTLSDR_SOURCES
    ${RTLSDR_SRC}/librtlsdr.c
    ${RTLSDR_SRC}/tuner_e4k.c
    ${RTLSDR_SRC}/tuner_fc0012.c
    ${RTLSDR_SRC}/tuner_fc0013.c
    ${RTLSDR_SRC}/tuner_fc2580.c
    ${RTLSDR_SRC}/tuner_r82xx.c
)

# Core rtl_433 source files (same set as r_433 in src/CMakeLists.txt)
set(RTL433_CORE_SOURCES
    ${RTL433_SRC}/abuf.c
    ${RTL433_SRC}/am_analyze.c
    ${RTL433_SRC}/baseband.c
    ${RTL433_SRC}/bit_util.c
    ${RTL433_SRC}/bitbuffer.c
    ${RTL433_SRC}/compat_paths.c
    ${RTL433_SRC}/compat_time.c
    ${RTL433_SRC}/confparse.c
    ${RTL433_SRC}/data.c
    ${RTL433_SRC}/data_tag.c
    ${RTL433_SRC}/decoder_util.c
    ${RTL433_SRC}/fileformat.c
    ${RTL433_SRC}/http_server.c
    ${RTL433_SRC}/jsmn.c
    ${RTL433_SRC}/list.c
    ${RTL433_SRC}/logger.c
    ${RTL433_SRC}/mongoose.c
    ${RTL433_SRC}/optparse.c
    ${RTL433_SRC}/output_file.c
    ${RTL433_SRC}/output_influx.c
    ${RTL433_SRC}/output_log.c
    ${RTL433_SRC}/output_mqtt.c
    ${RTL433_SRC}/output_rtltcp.c
    ${RTL433_SRC}/output_trigger.c
    ${RTL433_SRC}/output_udp.c
    ${RTL433_SRC}/pulse_analyzer.c
    ${RTL433_SRC}/pulse_data.c
    ${RTL433_SRC}/pulse_detect.c
    ${RTL433_SRC}/pulse_detect_fsk.c
    ${RTL433_SRC}/pulse_slicer.c
    ${RTL433_SRC}/r_api.c
    ${RTL433_SRC}/r_util.c
    ${RTL433_SRC}/raw_output.c
    ${RTL433_SRC}/rfraw.c
    ${RTL433_SRC}/samp_grab.c
    ${RTL433_SRC}/sdr.c
    ${RTL433_SRC}/term_ctl.c
    ${RTL433_SRC}/write_sigrok.c
    # device decoders
    ${RTL433_SRC}/devices/abmt.c
    ${RTL433_SRC}/devices/acurite.c
    ${RTL433_SRC}/devices/acurite_01185m.c
    ${RTL433_SRC}/devices/akhan_100F14.c
    ${RTL433_SRC}/devices/alecto.c
    ${RTL433_SRC}/devices/ambient_weather.c
    ${RTL433_SRC}/devices/ambientweather_tx8300.c
    ${RTL433_SRC}/devices/ambientweather_wh31e.c
    ${RTL433_SRC}/devices/ant_antplus.c
    ${RTL433_SRC}/devices/apator_metra_eitn30.c
    ${RTL433_SRC}/devices/apator_metra_erm30.c
    ${RTL433_SRC}/devices/arad_ms_meter.c
    ${RTL433_SRC}/devices/archos_tbh.c
    ${RTL433_SRC}/devices/arexx_ml.c
    ${RTL433_SRC}/devices/atech_ws308.c
    ${RTL433_SRC}/devices/auriol_4ld5661.c
    ${RTL433_SRC}/devices/auriol_aft77b2.c
    ${RTL433_SRC}/devices/auriol_afw2a1.c
    ${RTL433_SRC}/devices/auriol_ahfl.c
    ${RTL433_SRC}/devices/auriol_hg02832.c
    ${RTL433_SRC}/devices/badger_orion_endpoint.c
    ${RTL433_SRC}/devices/badger_water.c
    ${RTL433_SRC}/devices/baldr_rain.c
    ${RTL433_SRC}/devices/baldr_therm.c
    ${RTL433_SRC}/devices/blueline.c
    ${RTL433_SRC}/devices/blyss.c
    ${RTL433_SRC}/devices/bm5.c
    ${RTL433_SRC}/devices/brennenstuhl_rcs_2044.c
    ${RTL433_SRC}/devices/bresser_3ch.c
    ${RTL433_SRC}/devices/bresser_5in1.c
    ${RTL433_SRC}/devices/bresser_6in1.c
    ${RTL433_SRC}/devices/bresser_7in1.c
    ${RTL433_SRC}/devices/bresser_leakage.c
    ${RTL433_SRC}/devices/bresser_lightning.c
    ${RTL433_SRC}/devices/bresser_st1005h.c
    ${RTL433_SRC}/devices/bt_rain.c
    ${RTL433_SRC}/devices/burnhardbbq.c
    ${RTL433_SRC}/devices/calibeur.c
    ${RTL433_SRC}/devices/cardin.c
    ${RTL433_SRC}/devices/cavius.c
    ${RTL433_SRC}/devices/ced7000.c
    ${RTL433_SRC}/devices/celsia_czc1.c
    ${RTL433_SRC}/devices/chamberlain_cwpirc.c
    ${RTL433_SRC}/devices/chuango.c
    ${RTL433_SRC}/devices/cmr113.c
    ${RTL433_SRC}/devices/companion_wtr001.c
    ${RTL433_SRC}/devices/cotech_36_7959.c
    ${RTL433_SRC}/devices/current_cost.c
    ${RTL433_SRC}/devices/danfoss.c
    ${RTL433_SRC}/devices/deltadore_x3d.c
    ${RTL433_SRC}/devices/digitech_xc0324.c
    ${RTL433_SRC}/devices/directv.c
    ${RTL433_SRC}/devices/dish_remote_6_3.c
    ${RTL433_SRC}/devices/dsc.c
    ${RTL433_SRC}/devices/ec3k.c
    ${RTL433_SRC}/devices/ecodhome.c
    ${RTL433_SRC}/devices/ecowitt.c
    ${RTL433_SRC}/devices/efergy_e2_classic.c
    ${RTL433_SRC}/devices/efergy_optical.c
    ${RTL433_SRC}/devices/efth800.c
    ${RTL433_SRC}/devices/elro_db286a.c
    ${RTL433_SRC}/devices/elv.c
    ${RTL433_SRC}/devices/emax.c
    ${RTL433_SRC}/devices/emontx.c
    ${RTL433_SRC}/devices/emos_e6016.c
    ${RTL433_SRC}/devices/emos_e6016_rain.c
    ${RTL433_SRC}/devices/enocean_erp1.c
    ${RTL433_SRC}/devices/ert_idm.c
    ${RTL433_SRC}/devices/ert_scm.c
    ${RTL433_SRC}/devices/esa.c
    ${RTL433_SRC}/devices/esic_emt7110.c
    ${RTL433_SRC}/devices/esperanza_ews.c
    ${RTL433_SRC}/devices/eurochron.c
    ${RTL433_SRC}/devices/fineoffset.c
    ${RTL433_SRC}/devices/fineoffset_wh1050.c
    ${RTL433_SRC}/devices/fineoffset_wh1080.c
    ${RTL433_SRC}/devices/fineoffset_wh31l.c
    ${RTL433_SRC}/devices/fineoffset_wh43.c
    ${RTL433_SRC}/devices/fineoffset_wh45.c
    ${RTL433_SRC}/devices/fineoffset_wh46.c
    ${RTL433_SRC}/devices/fineoffset_wh55.c
    ${RTL433_SRC}/devices/fineoffset_wn34.c
    ${RTL433_SRC}/devices/fineoffset_ws80.c
    ${RTL433_SRC}/devices/fineoffset_ws85.c
    ${RTL433_SRC}/devices/fineoffset_ws90.c
    ${RTL433_SRC}/devices/flex.c
    ${RTL433_SRC}/devices/flowis.c
    ${RTL433_SRC}/devices/fordremote.c
    ${RTL433_SRC}/devices/fs20.c
    ${RTL433_SRC}/devices/ft004b.c
    ${RTL433_SRC}/devices/funkbus.c
    ${RTL433_SRC}/devices/gasmate_ba1008.c
    ${RTL433_SRC}/devices/ge_coloreffects.c
    ${RTL433_SRC}/devices/geevon.c
    ${RTL433_SRC}/devices/geevon_tx19.c
    ${RTL433_SRC}/devices/generic_motion.c
    ${RTL433_SRC}/devices/generic_remote.c
    ${RTL433_SRC}/devices/generic_temperature_sensor.c
    ${RTL433_SRC}/devices/geo_minim.c
    ${RTL433_SRC}/devices/govee.c
    ${RTL433_SRC}/devices/govee_h5059.c
    ${RTL433_SRC}/devices/gridstream.c
    ${RTL433_SRC}/devices/gt_tmbbq05.c
    ${RTL433_SRC}/devices/gt_wt_02.c
    ${RTL433_SRC}/devices/gt_wt_03.c
    ${RTL433_SRC}/devices/hcs200.c
    ${RTL433_SRC}/devices/hideki.c
    ${RTL433_SRC}/devices/holman_ws5029.c
    ${RTL433_SRC}/devices/homelead_hg9901.c
    ${RTL433_SRC}/devices/hondaremote.c
    ${RTL433_SRC}/devices/honeywell.c
    ${RTL433_SRC}/devices/honeywell_cm921.c
    ${RTL433_SRC}/devices/honeywell_wdb.c
    ${RTL433_SRC}/devices/ht680.c
    ${RTL433_SRC}/devices/ibis_beacon.c
    ${RTL433_SRC}/devices/ikea_sparsnas.c
    ${RTL433_SRC}/devices/infactory.c
    ${RTL433_SRC}/devices/inkbird_ith20r.c
    ${RTL433_SRC}/devices/inovalley-kw9015b.c
    ${RTL433_SRC}/devices/insteon.c
    ${RTL433_SRC}/devices/interlogix.c
    ${RTL433_SRC}/devices/intertechno.c
    ${RTL433_SRC}/devices/jasco.c
    ${RTL433_SRC}/devices/kedsum.c
    ${RTL433_SRC}/devices/kerui.c
    ${RTL433_SRC}/devices/klimalogg.c
    ${RTL433_SRC}/devices/lacrosse_breezepro.c
    ${RTL433_SRC}/devices/lacrosse.c
    ${RTL433_SRC}/devices/lacrosse_r1.c
    ${RTL433_SRC}/devices/lacrosse_th3.c
    ${RTL433_SRC}/devices/lacrosse_tx141x.c
    ${RTL433_SRC}/devices/lacrosse_tx31u.c
    ${RTL433_SRC}/devices/lacrosse_tx34.c
    ${RTL433_SRC}/devices/lacrosse_tx35.c
    ${RTL433_SRC}/devices/lacrosse_wr1.c
    ${RTL433_SRC}/devices/lacrosse_ws7000.c
    ${RTL433_SRC}/devices/lacrossews.c
    ${RTL433_SRC}/devices/lightwave_rf.c
    ${RTL433_SRC}/devices/markisol.c
    ${RTL433_SRC}/devices/marlec_solar.c
    ${RTL433_SRC}/devices/maverick_et73.c
    ${RTL433_SRC}/devices/maverick_et73x.c
    ${RTL433_SRC}/devices/maverick_xr30.c
    ${RTL433_SRC}/devices/maverick_xr50.c
    ${RTL433_SRC}/devices/m_bus.c
    ${RTL433_SRC}/devices/mebus.c
    ${RTL433_SRC}/devices/megacode.c
    ${RTL433_SRC}/devices/missil_ml0757.c
    ${RTL433_SRC}/devices/mueller_hotrod.c
    ${RTL433_SRC}/devices/neptune_r900.c
    ${RTL433_SRC}/devices/newkaku.c
    ${RTL433_SRC}/devices/new_template.c
    ${RTL433_SRC}/devices/nexa.c
    ${RTL433_SRC}/devices/nexus.c
    ${RTL433_SRC}/devices/nice_flor_s.c
    ${RTL433_SRC}/devices/norgo.c
    ${RTL433_SRC}/devices/oil_smart.c
    ${RTL433_SRC}/devices/oil_standard.c
    ${RTL433_SRC}/devices/oil_watchman_advanced.c
    ${RTL433_SRC}/devices/oil_watchman.c
    ${RTL433_SRC}/devices/opus_xt300.c
    ${RTL433_SRC}/devices/oregon_scientific.c
    ${RTL433_SRC}/devices/oregon_scientific_sl109h.c
    ${RTL433_SRC}/devices/oregon_scientific_v1.c
    ${RTL433_SRC}/devices/oria_wa150km.c
    ${RTL433_SRC}/devices/philips_aj3650.c
    ${RTL433_SRC}/devices/philips_aj7010.c
    ${RTL433_SRC}/devices/proflame2.c
    ${RTL433_SRC}/devices/prologue.c
    ${RTL433_SRC}/devices/proove.c
    ${RTL433_SRC}/devices/quhwa.c
    ${RTL433_SRC}/devices/quinetic.c
    ${RTL433_SRC}/devices/radiohead_ask.c
    ${RTL433_SRC}/devices/rainpoint.c
    ${RTL433_SRC}/devices/rainpoint_hcs012arf.c
    ${RTL433_SRC}/devices/regency_fan.c
    ${RTL433_SRC}/devices/revolt_nc5462.c
    ${RTL433_SRC}/devices/revolt_zx7717.c
    ${RTL433_SRC}/devices/rftech.c
    ${RTL433_SRC}/devices/risco_agility.c
    ${RTL433_SRC}/devices/rojaflex.c
    ${RTL433_SRC}/devices/rosstech_dcu706.c
    ${RTL433_SRC}/devices/rubicson_48659.c
    ${RTL433_SRC}/devices/rubicson.c
    ${RTL433_SRC}/devices/rubicson_pool_48942.c
    ${RTL433_SRC}/devices/s3318p.c
    ${RTL433_SRC}/devices/sainlogic_sa8.c
    ${RTL433_SRC}/devices/schou_72543_rain.c
    ${RTL433_SRC}/devices/schraeder.c
    ${RTL433_SRC}/devices/scmplus.c
    ${RTL433_SRC}/devices/secplus_v1.c
    ${RTL433_SRC}/devices/secplus_v2.c
    ${RTL433_SRC}/devices/sharp_spc775.c
    ${RTL433_SRC}/devices/silvercrest.c
    ${RTL433_SRC}/devices/simplisafe.c
    ${RTL433_SRC}/devices/simplisafe_gen3.c
    ${RTL433_SRC}/devices/smoke_gs558.c
    ${RTL433_SRC}/devices/solight_te44.c
    ${RTL433_SRC}/devices/somfy_iohc.c
    ${RTL433_SRC}/devices/somfy_rts.c
    ${RTL433_SRC}/devices/springfield.c
    ${RTL433_SRC}/devices/srsmith_pool_srs_2c_tx.c
    ${RTL433_SRC}/devices/steelmate.c
    ${RTL433_SRC}/devices/telldus_ft0385r.c
    ${RTL433_SRC}/devices/tfa_14_1504_v2.c
    ${RTL433_SRC}/devices/tfa_30_3196.c
    ${RTL433_SRC}/devices/tfa_30_3221.c
    ${RTL433_SRC}/devices/tfa_drop_30.3233.c
    ${RTL433_SRC}/devices/tfa_marbella.c
    ${RTL433_SRC}/devices/tfa_pool_thermometer.c
    ${RTL433_SRC}/devices/tfa_twin_plus_30.3049.c
    ${RTL433_SRC}/devices/thermopro_tp11.c
    ${RTL433_SRC}/devices/thermopro_tp12.c
    ${RTL433_SRC}/devices/thermopro_tp211b.c
    ${RTL433_SRC}/devices/thermopro_tp28b.c
    ${RTL433_SRC}/devices/thermopro_tp82xb.c
    ${RTL433_SRC}/devices/thermopro_tp862b.c
    ${RTL433_SRC}/devices/thermopro_tx2.c
    ${RTL433_SRC}/devices/thermopro_tx2c.c
    ${RTL433_SRC}/devices/thermopro_tx7b.c
    ${RTL433_SRC}/devices/thermor.c
    ${RTL433_SRC}/devices/tpms_abarth124.c
    ${RTL433_SRC}/devices/tpms_airpuxem.c
    ${RTL433_SRC}/devices/tpms_ave.c
    ${RTL433_SRC}/devices/tpms_bmw.c
    ${RTL433_SRC}/devices/tpms_bmw_g3.c
    ${RTL433_SRC}/devices/tpms_citroen.c
    ${RTL433_SRC}/devices/tpms_eezrv.c
    ${RTL433_SRC}/devices/tpms_elantra2012.c
    ${RTL433_SRC}/devices/tpms_ford.c
    ${RTL433_SRC}/devices/tpms_gm.c
    ${RTL433_SRC}/devices/tpms_hyundai_vdo.c
    ${RTL433_SRC}/devices/tpms_jansite.c
    ${RTL433_SRC}/devices/tpms_jansite_solar.c
    ${RTL433_SRC}/devices/tpms_kia.c
    ${RTL433_SRC}/devices/tpms_nissan.c
    ${RTL433_SRC}/devices/tpms_pmv107j.c
    ${RTL433_SRC}/devices/tpms_porsche.c
    ${RTL433_SRC}/devices/tpms_renault_0435r.c
    ${RTL433_SRC}/devices/tpms_renault.c
    ${RTL433_SRC}/devices/tpms_toyota.c
    ${RTL433_SRC}/devices/tpms_truck.c
    ${RTL433_SRC}/devices/tpms_trw.c
    ${RTL433_SRC}/devices/tpms_tyreguard400.c
    ${RTL433_SRC}/devices/ts_ft002.c
    ${RTL433_SRC}/devices/ttx201.c
    ${RTL433_SRC}/devices/universalfanctrl.c
    ${RTL433_SRC}/devices/vaillant_vrt340f.c
    ${RTL433_SRC}/devices/vauno_en8822c.c
    ${RTL433_SRC}/devices/vevor_7in1.c
    ${RTL433_SRC}/devices/visonic_powercode.c
    ${RTL433_SRC}/devices/wallarge_cltx001.c
    ${RTL433_SRC}/devices/watts_thermostat.c
    ${RTL433_SRC}/devices/waveman.c
    ${RTL433_SRC}/devices/wec2103.c
    ${RTL433_SRC}/devices/wg_pb12v1.c
    ${RTL433_SRC}/devices/ws2032.c
    ${RTL433_SRC}/devices/wssensor.c
    ${RTL433_SRC}/devices/wt0124.c
    ${RTL433_SRC}/devices/wt450.c
    ${RTL433_SRC}/devices/x10_rf.c
    ${RTL433_SRC}/devices/x10_sec.c
    ${RTL433_SRC}/devices/yale_hsa.c
)

# FFI wrapper
set(FFI_SOURCES
    ${RTL433_SRC}/rtl433_ffi.c
)

add_library(rtl433_ffi SHARED
    ${LIBUSB_SOURCES}
    ${RTLSDR_SOURCES}
    ${RTL433_CORE_SOURCES}
    ${FFI_SOURCES}
)

target_include_directories(rtl433_ffi PRIVATE
    ${RTL433_INC}
    ${RTLSDR_INC}
    ${LIBUSB_SRC}
    ${LIBUSB_ROOT}/android
)

# Suppress warnings in the bundled mongoose networking library
set_source_files_properties(${RTL433_SRC}/mongoose.c
    PROPERTIES COMPILE_FLAGS "-w")

set_target_properties(rtl433_ffi PROPERTIES C_STANDARD 99)

# Android system libraries
find_library(log-lib log)
target_link_libraries(rtl433_ffi
    ${log-lib}
    m
)
