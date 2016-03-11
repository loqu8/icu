# Copyright (c) 1999-2014, International Business Machines Corporation and
# others. All Rights Reserved.
# A list of UCM's to build
# Note: 
#
#   If you are thinking of modifying this file, READ THIS. 
#
# Instead of changing this file [unless you want to check it back in],
# you should consider creating a 'ucmlocal.mk' file in this same directory.
# Then, you can have your local changes remain even if you upgrade or re
# configure the ICU.
#
# Example 'ucmlocal.mk' files:
#
#  * To add an additional converter to the list: 
#    _____________________________________________________
#    |  UCM_SOURCE_LOCAL =  myconverter.ucm ...
#
#  * To add EBCDIC converters that uses different SI/SO characters:
#    Note: ICU is updated to handle three EBCDIC converters (JIPS, KEIS, and JEF) which uses 
#          different SI/SO characters than the "standard" ones:  0xf/0xe.
#          This behaviour is done in the mbcs converter code and is turned on if for that converter
#          if the name contains either JIPS, KEIS, or JEF. (e.g. my_KEIS_converter.ucm) 
#    _____________________________________________________
#    |  UCM_SOURCE_EBCDIC_IGNORE_SISO_LOCAL = my_KEIS_converter.ucm
#
#  * To REPLACE the default list and only build with a few
#     converters:
#    _____________________________________________________
#    |  UCM_SOURCE = ibm-913.ucm ibm-949.ucm ibm-37.ucm
#
# If you are planning to exclude EBCDIC mappings in you data then please delete
# ucmebcdic.mk from the <icu>/source/data directory
#

UCM_SOURCE_FILES = 
