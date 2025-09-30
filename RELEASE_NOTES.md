# Release Notes

## Bug Fixes

### Compiler Warnings Fixed
- **Fixed dangling-else warnings in unicast_monit.c**: Added explicit braces around `if` statements in both `unicast_send_json_state()` and `unicast_send_xml_state()` functions to eliminate compiler warnings about ambiguous `else` clauses.

#### Details:
- Fixed warning at line 413 in `unicast_send_json_state()` function
- Fixed warning at line 786 in `unicast_send_xml_state()` function
- Both fixes involve the frontend type detection logic for `FE_OFDM` (DVB-T/DVB-T2) handling
- Added proper indentation for the `#else` branch in the preprocessor conditional blocks
- No functional changes - only improved code clarity and eliminated compiler warnings

#### Technical Changes:
- Wrapped `if (strengthparams->tune_p->fe_type==FE_OFDM)` statements with explicit braces `{}`
- Fixed indentation of `snprintf(fetype,10,"DVB-T");` in the `#else` branch
- Maintained identical logic flow and functionality

These changes improve code maintainability and eliminate compiler warnings without affecting the runtime behavior of the application.
