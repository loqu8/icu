# *   Copyright (C) 1998-2016, International Business Machines
# *   Corporation and others.  All Rights Reserved.
RBNF_CLDR_VERSION = %version%
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
#    |  RBNF_SOURCE_LOCAL =   myLocale.txt ...
#
#  * To REPLACE the default list and only build with a few
#    locales:
#    _____________________________________________________
#    |  RBNF_SOURCE = ar.txt ar_AE.txt en.txt de.txt zh.txt
#
#
# Generated by LDML2ICUConverter, from LDML source files.

# Aliases without a corresponding xx.xml file (see icu-config.xml & build.xml)
RBNF_SYNTHETIC_ALIAS = es_DO.txt es_GT.txt es_HN.txt es_MX.txt\
 es_NI.txt es_PA.txt es_PR.txt es_SV.txt es_US.txt\
 in.txt iw.txt no.txt sh.txt zh_HK.txt\
 zh_Hant_HK.txt zh_MO.txt zh_TW.txt


# All aliases (to not be included under 'installed'), but not including root.
RBNF_ALIAS_SOURCE = $(RBNF_SYNTHETIC_ALIAS)


# Ordinary resources
RBNF_SOURCE = 

