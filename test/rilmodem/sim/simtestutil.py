#!/usr/bin/python3
#
#  oFono - Open Source Telephony - RIL Modem test
#
#  Copyright (C) 2014 Canonical Ltd.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License version 2 as
#  published by the Free Software Foundation.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#
# This test ensures that basic modem information is available
# when the modem is online and has a valid, unlocked SIM present.

"""SIM Test utility functions.

This module contains functions used by the SIM functional test scripts.

ToDo:
 * Add check for optional SimManager 'PreferredLanguage' property
"""

import argparse
import dbus
import subprocess
import sys
import unittest

emergency_numbers = ["08", "000", "999", "110", "112", "911", "118", "119"]

# Note - rilmodem doesn't remove 'rat' when modem is offlined;
# as this is different behavior than when booted offline, "rat"
# is not present in either of the following offline lists

no_sim_offline_features = [ "sim" ]
sim_offline_features = [ "sim", "gprs", "sms" ]

no_sim_online_features = [ "rat", "sim" ]
sim_online_features = [ "gprs", "ussd", "net", "sms", "rat", "sim" ]

no_sim_offline_ifaces = [ "org.ofono.CallVolume",
			"org.ofono.VoiceCallManager" ]

# LP: #1399756;  RadioSettings isn't properly removed from
# 'Interfaces' ( and correspondingly "rat" from 'Features' )
# when rilmodem is set offline.  Neither are present if the
# device boots with modems offline. That's why it's not
# included in this list.
sim_offline_ifaces = [ "org.ofono.ConnectionManager",
			"org.ofono.Phonebook",
			"org.ofono.CallForwarding",
			"org.ofono.SmartMessaging",
			"org.ofono.PushNotification",
			"org.ofono.MessageManager",
			"org.ofono.NetworkTime",
			"org.ofono.MessageWaiting",
			"org.ofono.SimManager",
			"org.ofono.CallVolume",
			"org.ofono.VoiceCallManager" ]

no_sim_online_ifaces = [ "org.ofono.RadioSettings",
			"org.ofono.SimManager",
			"org.ofono.CallVolume",
			"org.ofono.VoiceCallManager" ]

sim_online_ifaces = [ "org.ofono.ConnectionManager",
			"org.ofono.CallBarring",
			"org.ofono.CallSettings",
			"org.ofono.SupplementaryServices",
			"org.ofono.NetworkRegistration",
			"org.ofono.Phonebook",
			"org.ofono.CallForwarding",
			"org.ofono.SmartMessaging",
			"org.ofono.PushNotification",
			"org.ofono.MessageManager",
			"org.ofono.NetworkTime",
			"org.ofono.MessageWaiting",
			"org.ofono.RadioSettings",
			"org.ofono.SimManager",
			"org.ofono.CallVolume",
			"org.ofono.VoiceCallManager" ]

modem_properties = [ "Revision", "Serial", "Model",
		"Features", "Online", "Type",
		"Interfaces", "Emergency", "Manufacturer",
		"Powered", "Lockdown" ]

sim_properties = [ "Present", "FixedDialing", "BarredDialing",
			"SubscriberNumbers", "LockedPins",
			"PinRequired", "Retries" ]

def get_product():
	product_bytes = subprocess.check_output(["getprop", "ro.build.product"])
	return product_bytes.decode('utf-8').strip('\n')

def sim_unittest_main(args):

	if args.debug:
		print(args)
		print(sys.argv[0])

	if len(args.unittest_args) == 0:
		args.unittest_args.append("sys.argv[0]")
		unittest.main(argv=args.unittest_args)
	else:
		unittest.main()

def parse_args(parser=None):

	if parser is None:
		parser = argparse.ArgumentParser()

	parser.add_argument("-d",
			"--debug",
			dest="debug",
			action="store_true",
			help="""Enables debug verbosity""",
			)

	parser.add_argument("-m",
			"--modem",
			dest="modem",
			action="store",
			help="""Specifies modem path""",
			)

	parser.add_argument("unittest_args", nargs="*")

	parser.set_defaults(modem="")

	args = parser.parse_args()

	return args

class SimTestCase(unittest.TestCase):

	def setUp(self):
		self.bus = dbus.SystemBus()

		self.manager = dbus.Interface(self.bus.get_object('org.ofono',
									'/'),
							'org.ofono.Manager')
		self.modems = self.manager.GetModems()

	def if_supports_sim_offline(self):
		if self.product != "krillin":
			return True
		else:
			return False

	def check_no_sim_present(self, path):

		# valid SimManager properties
		simmanager = dbus.Interface(self.bus.get_object('org.ofono',
								path),
							'org.ofono.SimManager')

		properties = simmanager.GetProperties()

		self.assertTrue(properties["Present"] != 1)

	def validate_modem_properties(self, path, test_online, test_sims):

		modem = dbus.Interface(self.bus.get_object('org.ofono', path),
					'org.ofono.Modem')
		properties = modem.GetProperties()

		if self.args.debug == True:
			print("[ %s ]" % path)

		for property in modem_properties:
			self.assertTrue(property in properties)

		self.assertTrue(properties["Revision"] != "ValueError")
		self.assertTrue(properties["Type"] == "hardware")
		self.assertTrue(properties["Manufacturer"] == "Fake Manufacturer")
		self.assertTrue(properties["Model"] == "Fake Modem Model")
		self.assertTrue(properties["Powered"] == 1)
		self.assertTrue(properties["Emergency"] == 0)
		self.assertTrue(properties["Lockdown"] == 0)

		# emulator == "goldfish"
		#
		if self.product == "goldfish":
			self.assertTrue(properties["Serial"] == "000000000000000")
		else:
			self.assertTrue(properties["Serial"] != "")

		self.assertTrue(properties["Online"] == test_online)

		# test features
		features = properties["Features"]

		if test_online:
			if test_sims:
                                check_features = sim_online_features[:]
			else:
                                check_features = no_sim_online_features[:]
		else:
			if self.product == "krillin":
				check_features = []
			else:
				if test_sims:
					check_features = sim_offline_features[:]
				else:
					check_features = no_sim_offline_features[:]

		for feature in check_features:
			if self.args.debug:
				print(feature)

			self.assertTrue(feature in features)

		# test interfaces
		ifaces = properties["Interfaces"]

		if test_online:
			if test_sims:
				check_ifaces = sim_online_ifaces[:]

				if self.product == "krillin":
					check_ifaces.append(
						"org.ofono.MtkSettings")
			else:
				check_ifaces = no_sim_online_ifaces[:]

				# LP: #1399746; NetworkTime is only
				# available on krillin in this state,
				# and shouldn't be without a SIM present

				if self.product == "krillin":
					check_ifaces.append(
						"org.ofono.NetworkTime")
					check_ifaces.append(
						"org.ofono.MtkSettings")
		else:

			# krillin no diff between sim/no-SIM when offline
			if self.product == "krillin":
				check_ifaces = no_sim_offline_ifaces[:]
				check_ifaces.append("org.ofono.NetworkTime")
			else:
				if test_sims:
					check_ifaces = sim_offline_ifaces[:]
				else:
					check_ifaces = no_sim_offline_ifaces[:]
					check_ifaces.append("org.ofono.SimManager")

		for iface in check_ifaces:
			if self.args.debug:
				print(iface)

			self.assertTrue(iface in ifaces)

		return modem

	def validate_call_volume_properties(self, path):

		call_volume = dbus.Interface(self.bus.get_object('org.ofono',
									path),
						'org.ofono.CallVolume')
		properties = call_volume.GetProperties()
		keys = list(properties.keys())

		self.assertTrue(properties["MicrophoneVolume"] == 0)
		self.assertTrue(properties["SpeakerVolume"] == 0)

		# The value of 'Muted' differs on mako/krillin: see
		# bug for details:
		#
		# https://bugs.launchpad.net/ubuntu/+source/ofono/+bug/1396317

		if self.product == "krillin":
			self.assertTrue(properties["Muted"] == 0)
		else:
			self.assertTrue(properties["Muted"] == 1)

		self.assertTrue(keys.index("Muted") != "ValueError")

	def validate_emergency_numbers(self, path):
		# valid VoiceCallManager properties
		voice = dbus.Interface(self.bus.get_object('org.ofono', path),
						'org.ofono.VoiceCallManager')

		properties = voice.GetProperties()
		keys = list(properties.keys())

		self.assertTrue(keys.index("EmergencyNumbers") != "ValueError")

		numbers = properties["EmergencyNumbers"]

		if self.args.debug:
			print("%s" % numbers)

		for number in numbers:
			self.assertTrue(number in emergency_numbers)

	def validate_sim_properties(self, path):

		# valid SimManager properties
		simmanager = dbus.Interface(self.bus.get_object('org.ofono',
								path),
							'org.ofono.SimManager')

		properties = simmanager.GetProperties()
		keys = list(properties.keys())

		for property in sim_properties:
			self.assertTrue(property in keys)

		self.assertTrue(properties["Present"] == 1)

		# Don't validate "Retries"; it's only populated
		# after failed PIN/PUK unlock attempts

		self.assertTrue(properties["BarredDialing"] == 0)
		self.assertTrue(properties["FixedDialing"] == 0)

		# emulator == "goldfish"
		if self.product == "goldfish":
			self.assertTrue(properties["SubscriberNumbers"][0]
					== "15555215554")

		self.assertTrue(len(properties["LockedPins"]) == 0)
		self.assertTrue(properties["PinRequired"] == "none")

                # validate optional properties

		if "CardIdentifier" in keys and self.product == "goldfish":
			self.assertTrue(properties["CardIdentifier"]
					== "89014103211118510720")

		if "MobileCountryCode" in keys:
			if self.product == "goldfish":
				self.assertTrue(properties["MobileCountryCode"]
						 == "310")
			elif self.args.mcc != "":
				self.assertTrue(properties["MobileCountryCode"]
						 == args.mcc)

		if "MobileNetworkCode" in keys:
			if self.product == "goldfish":
				self.assertTrue(properties["MobileNetworkCode"]
						 == "260")
			elif self.args.mnc != "":
				self.assertTrue(properties["MobileNetworkCode"]
						 == args.mnc)

		if "SubscriberIdentity" in keys:
			if self.product == "goldfish":
				self.assertTrue(properties["SubscriberIdentity"]
						 == "310260000000000")
			elif self.args.subscriber != "":
				self.assertTrue(properties["SubscriberIdentity"]
						 == args.subscriber)


	def main(self, args):
		self.args = args
		self.product = get_product()

		if args.debug:
			print ("ro.build.product: %s" % self.product)

		if len(args.modem) > 0:
			self.validate_modem(args.modem)
		else:
			for modem in self.modems:
				self.validate_modem(modem[0])
