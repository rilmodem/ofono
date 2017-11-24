'''Apport package hook for ConnMan

(c) 2010 Canonical Ltd.
Contributors:
Kalle Valo <kalle.valo@canonical.com>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2 of the License, or (at your
option) any later version.  See http://www.gnu.org/copyleft/gpl.html for
the full text of the license.
'''

import apport.hookutils

def add_info(report, ui):
    apport.hookutils.attach_network(report)
    apport.hookutils.attach_wifi(report)
    apport.hookutils.attach_hardware(report)
    if not apport.packaging.is_distro_package(report['Package'].split()[0]):
        report['ThirdParty'] = 'True'
        report['CrashDB'] = 'ofono'

if __name__ == '__main__':
    report = {}
    report['CrashDB'] = 'ubuntu'
    add_info(report, None)
    for key in report:
        print '%s: %s' % (key, report[key].split('\n', 1)[0])
