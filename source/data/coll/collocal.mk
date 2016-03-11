# *   Copyright (C) 1998-2016, International Business Machines
# *   Corporation and others.  All Rights Reserved.
COLLATION_CLDR_VERSION = %version%
# A list of txt's to build
# Note:
#
#   If you are thinking of modifying this file, READ THIS.
#
# Instead of changing this file [unless you want to check it back in],
# you should consider creating a '%local%' file in this same directory.
# Then, you can have your local changes remain even if you upgrade or
# reconfigure ICU.
#
# Example '%local%' files:
#
#  * To add an additional locale to the list:
#    _____________________________________________________
#    |  COLLATION_SOURCE_LOCAL =   myLocale.txt ...
#
#  * To REPLACE the default list and only build with a few
#    locales:
#    _____________________________________________________
#    |  COLLATION_SOURCE = ar.txt ar_AE.txt en.txt de.txt zh.txt
#
#
# Generated by LDML2ICUConverter, from LDML source files.

# Aliases without a corresponding xx.xml file (see icu-config.xml & build.xml)
COLLATION_SYNTHETIC_ALIAS = zh_CN.txt zh_HK.txt zh_Hans.txt\
 zh_Hans_CN.txt zh_Hans_SG.txt zh_Hant_HK.txt zh_Hant_MO.txt zh_Hant_TW.txt\
 zh_MO.txt zh_SG.txt zh_TW.txt


# All aliases (to not be included under 'installed'), but not including root.
COLLATION_ALIAS_SOURCE = $(COLLATION_SYNTHETIC_ALIAS)


# Empty locales, used for validSubLocale fallback.
COLLATION_EMPTY_SOURCE =


# Ordinary resources
COLLATION_SOURCE = $(COLLATION_EMPTY_SOURCE) zh.txt zh_Hant.txt
