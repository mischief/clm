/* Shim: clm core includes <cjson/cJSON.h>, but ESP-IDF's "json" component
 * only installs a flat <cJSON.h>. Redirect here instead of patching core. */
#include <cJSON.h>
