/**************************************************************************
*
* Copyright (C) 2007 Steve Karg <skarg@users.sourceforge.net>
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*********************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bacdef.h"
#include "bacdcode.h"
#include "bacstr.h"
#include "bacenum.h"
#include "apdu.h"
#include "dcc.h"
#include "datalink.h"
#include "rs485.h"
#include "version.h"
#include "nvdata.h"
#include "stack.h"
#include "handlers.h"
/* objects */
#include "device.h"
#include "ai.h"
#include "av.h"
#include "bi.h"
#include "bo.h"

/* forward prototype */
int Device_Read_Property_Local(
    BACNET_READ_PROPERTY_DATA * rpdata);
bool Device_Write_Property_Local(
    BACNET_WRITE_PROPERTY_DATA * wp_data);

static struct object_functions {
    BACNET_OBJECT_TYPE Object_Type;
    object_init_function Object_Init;
    object_count_function Object_Count;
    object_index_to_instance_function Object_Index_To_Instance;
    object_valid_instance_function Object_Valid_Instance;
    object_name_function Object_Name;
    read_property_function Object_Read_Property;
    write_property_function Object_Write_Property;
    rpm_property_lists_function Object_RPM_List;
} Object_Table[] = {
    {
        OBJECT_DEVICE, NULL,    /* don't init - recursive! */
    Device_Count, Device_Index_To_Instance,
            Device_Valid_Object_Instance_Number, Device_Name,
            Device_Read_Property_Local, Device_Write_Property_Local,
            Device_Property_Lists}, {
    OBJECT_ANALOG_INPUT, Analog_Input_Init, Analog_Input_Count,
            Analog_Input_Index_To_Instance, Analog_Input_Valid_Instance,
            Analog_Input_Name, Analog_Input_Read_Property, NULL,
            Analog_Input_Property_Lists}, {
    OBJECT_ANALOG_VALUE, Analog_Value_Init, Analog_Value_Count,
            Analog_Value_Index_To_Instance, Analog_Value_Valid_Instance,
            Analog_Value_Name, Analog_Value_Read_Property,
            Analog_Value_Write_Property, Analog_Value_Property_Lists}, {
    OBJECT_BINARY_INPUT, Binary_Input_Init, Binary_Input_Count,
            Binary_Input_Index_To_Instance, Binary_Input_Valid_Instance,
            Binary_Input_Name, Binary_Input_Read_Property, NULL,
            Binary_Input_Property_Lists}, {
    OBJECT_BINARY_OUTPUT, Binary_Output_Init, Binary_Output_Count,
            Binary_Output_Index_To_Instance, Binary_Output_Valid_Instance,
            Binary_Output_Name, Binary_Output_Read_Property,
            Binary_Output_Write_Property, Binary_Output_Property_Lists}, {
    MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

/* note: you really only need to define variables for
   properties that are writable or that may change.
   The properties that are constant can be hard coded
   into the read-property encoding. */
static uint32_t Object_Instance_Number;
static BACNET_DEVICE_STATUS System_Status = STATUS_OPERATIONAL;

static BACNET_REINITIALIZED_STATE Reinitialize_State = BACNET_REINIT_IDLE;

/* These three arrays are used by the ReadPropertyMultiple handler */
static const int Device_Properties_Required[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_SYSTEM_STATUS,
    PROP_VENDOR_NAME,
    PROP_VENDOR_IDENTIFIER,
    PROP_MODEL_NAME,
    PROP_FIRMWARE_REVISION,
    PROP_APPLICATION_SOFTWARE_VERSION,
    PROP_PROTOCOL_VERSION,
    PROP_PROTOCOL_REVISION,
    PROP_PROTOCOL_SERVICES_SUPPORTED,
    PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED,
    PROP_OBJECT_LIST,
    PROP_MAX_APDU_LENGTH_ACCEPTED,
    PROP_SEGMENTATION_SUPPORTED,
    PROP_APDU_TIMEOUT,
    PROP_NUMBER_OF_APDU_RETRIES,
    PROP_MAX_MASTER,
    PROP_MAX_INFO_FRAMES,
    PROP_DEVICE_ADDRESS_BINDING,
    PROP_DATABASE_REVISION,
    -1
};

static const int Device_Properties_Optional[] = {
    PROP_DESCRIPTION,
    -1
};

static const int Device_Properties_Proprietary[] = {
    -1
};

static struct object_functions *Device_Objects_Find_Functions(
    BACNET_OBJECT_TYPE Object_Type)
{
    struct object_functions *pObject = NULL;

    pObject = &Object_Table[0];
    while (pObject->Object_Type < MAX_BACNET_OBJECT_TYPE) {
        /* handle each object type */
        if (pObject->Object_Type == Object_Type) {
            return (pObject);
        }

        pObject++;
    }

    return (NULL);
}

static int Read_Property_Common(
    struct object_functions *pObject,
    BACNET_READ_PROPERTY_DATA * rpdata)
{
    int apdu_len = -1;
    BACNET_CHARACTER_STRING char_string;
    char *pString = "";
    uint8_t *apdu = NULL;

    if ((rpdata->application_data == NULL) ||
        (rpdata->application_data_len == 0)) {
        return 0;
    }
    apdu = rpdata->application_data;
    switch (rpdata->object_property) {
        case PROP_OBJECT_IDENTIFIER:
            /* Device Object exception: requested instance
               may not match our instance if a wildcard */
            if (rpdata->object_type == OBJECT_DEVICE) {
                rpdata->object_instance = Object_Instance_Number;
            }
            apdu_len =
                encode_application_object_id(&apdu[0], rpdata->object_type,
                rpdata->object_instance);
            break;
        case PROP_OBJECT_NAME:
            if (pObject->Object_Name) {
                pString = pObject->Object_Name(rpdata->object_instance);
            }
            characterstring_init_ansi(&char_string, pString);
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_OBJECT_TYPE:
            apdu_len =
                encode_application_enumerated(&apdu[0], rpdata->object_type);
            break;
        default:
            if (pObject->Object_Read_Property) {
                apdu_len = pObject->Object_Read_Property(rpdata);
            }
            break;
    }

    return apdu_len;
}

/* Encodes the property APDU and returns the length,
   or sets the error, and returns -1 */
int Device_Read_Property(
    BACNET_READ_PROPERTY_DATA * rpdata)
{
    int apdu_len = BACNET_STATUS_ERROR;
    struct object_functions *pObject = NULL;

    /* initialize the default return values */
    pObject = Device_Objects_Find_Functions(rpdata->object_type);
    if (pObject) {
        if (pObject->Object_Valid_Instance &&
            pObject->Object_Valid_Instance(rpdata->object_instance)) {
            apdu_len = Read_Property_Common(pObject, rpdata);
        } else {
            rpdata->error_class = ERROR_CLASS_OBJECT;
            rpdata->error_code = ERROR_CODE_UNKNOWN_OBJECT;
        }
    } else {
        rpdata->error_class = ERROR_CLASS_OBJECT;
        rpdata->error_code = ERROR_CODE_UNSUPPORTED_OBJECT_TYPE;
    }

    return apdu_len;
}

bool Device_Write_Property(
    BACNET_WRITE_PROPERTY_DATA * wp_data)
{
    bool status = false;
    struct object_functions *pObject = NULL;

    /* initialize the default return values */
    pObject = Device_Objects_Find_Functions(wp_data->object_type);
    if (pObject) {
        if (pObject->Object_Valid_Instance &&
            pObject->Object_Valid_Instance(wp_data->object_instance)) {
            if (pObject->Object_Write_Property) {
                status = pObject->Object_Write_Property(wp_data);
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_WRITE_ACCESS_DENIED;
            }
        } else {
            wp_data->error_class = ERROR_CLASS_OBJECT;
            wp_data->error_code = ERROR_CODE_UNKNOWN_OBJECT;
        }
    } else {
        wp_data->error_class = ERROR_CLASS_OBJECT;
        wp_data->error_code = ERROR_CODE_UNSUPPORTED_OBJECT_TYPE;
    }

    return status;
}

static unsigned property_list_count(
    const int *pList)
{
    unsigned property_count = 0;

    if (pList) {
        while (*pList != -1) {
            property_count++;
            pList++;
        }
    }

    return property_count;
}

/* for a given object type, returns the special property list */
void Device_Objects_Property_List(
    BACNET_OBJECT_TYPE object_type,
    struct special_property_list_t *pPropertyList)
{
    struct object_functions *pObject = NULL;

    pPropertyList->Required.pList = NULL;
    pPropertyList->Optional.pList = NULL;
    pPropertyList->Proprietary.pList = NULL;

    /* If we can find an entry for the required object type
     * and there is an Object_List_RPM fn ptr then call it
     * to populate the pointers to the individual list counters.
     */

    pObject = Device_Objects_Find_Functions(object_type);
    if ((pObject != NULL) && (pObject->Object_RPM_List != NULL)) {
        pObject->Object_RPM_List(&pPropertyList->Required.pList,
            &pPropertyList->Optional.pList, &pPropertyList->Proprietary.pList);
    }

    /* Fetch the counts if available otherwise zero them */
    pPropertyList->Required.count =
        pPropertyList->Required.pList ==
        NULL ? 0 : property_list_count(pPropertyList->Required.pList);

    pPropertyList->Optional.count =
        pPropertyList->Optional.pList ==
        NULL ? 0 : property_list_count(pPropertyList->Optional.pList);

    pPropertyList->Proprietary.count =
        pPropertyList->Proprietary.pList ==
        NULL ? 0 : property_list_count(pPropertyList->Proprietary.pList);

    return;
}

void Device_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary)
{
    if (pRequired)
        *pRequired = Device_Properties_Required;
    if (pOptional)
        *pOptional = Device_Properties_Optional;
    if (pProprietary)
        *pProprietary = Device_Properties_Proprietary;

    return;
}

unsigned Device_Count(
    void)
{
    return 1;
}

uint32_t Device_Index_To_Instance(
    unsigned index)
{
    return Object_Instance_Number;
}

char *Device_Name(
    uint32_t object_instance)
{
    uint8_t encoding = 0;
    uint8_t length = 0;
    static char name[NV_EEPROM_DEVICE_NAME_SIZE + 1] = "";
    char *pName = NULL;

    if (object_instance == Object_Instance_Number) {
        eeprom_bytes_read(NV_EEPROM_DEVICE_NAME_ENCODING, &encoding, 1);
        eeprom_bytes_read(NV_EEPROM_DEVICE_NAME_LENGTH, &length, 1);
        eeprom_bytes_read(NV_EEPROM_DEVICE_NAME_0, (uint8_t *) & name,
            NV_EEPROM_DEVICE_NAME_SIZE);
        if ((encoding >= MAX_CHARACTER_STRING_ENCODING) ||
            (length > NV_EEPROM_DEVICE_NAME_SIZE) || (length < 1)) {
            encoding = CHARACTER_ANSI_X34;
            eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_ENCODING, &encoding, 1);
            sprintf(name, "DEVICE-%lu", Object_Instance_Number);
            eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_0, (uint8_t *) & name[0],
                NV_EEPROM_DEVICE_NAME_SIZE);
            length = strlen(name);
            eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_LENGTH, &length, 1);
        }
        if (length < NV_EEPROM_DEVICE_NAME_SIZE) {
            name[length] = 0;
        } else {
            name[NV_EEPROM_DEVICE_NAME_SIZE] = 0;
        }
        pName = &name[0];
    }

    return pName;
}

bool Device_Reinitialize(
    BACNET_REINITIALIZE_DEVICE_DATA * rd_data)
{
    bool status = false;

    if (characterstring_ansi_same(&rd_data->password, "rehmite")) {
        Reinitialize_State = rd_data->state;
        dcc_set_status_duration(COMMUNICATION_ENABLE, 0);
        /* Note: you could use a mix of state
           and password to multiple things */
        /* note: you probably want to restart *after* the
           simple ack has been sent from the return handler
           so just set a flag from here */
        status = true;
    } else {
        rd_data->error_class = ERROR_CLASS_SECURITY;
        rd_data->error_code = ERROR_CODE_PASSWORD_FAILURE;
    }

    return status;
}

void Device_Init(
    void)
{
    struct object_functions *pObject = NULL;

    pObject = &Object_Table[0];
    while (pObject->Object_Type < MAX_BACNET_OBJECT_TYPE) {
        if (pObject->Object_Init) {
            pObject->Object_Init();
        }
        pObject++;
    }
    dcc_set_status_duration(COMMUNICATION_ENABLE, 0);
    /* Get the data from the eeprom */
    eeprom_bytes_read(NV_EEPROM_DEVICE_0, (uint8_t *) & Object_Instance_Number,
        sizeof(Object_Instance_Number));
    if (Object_Instance_Number >= BACNET_MAX_INSTANCE) {
        Object_Instance_Number = 0;
        eeprom_bytes_write(NV_EEPROM_DEVICE_0,
            (uint8_t *) & Object_Instance_Number,
            sizeof(Object_Instance_Number));
    }
    (void) Device_Name(Object_Instance_Number);
}

/* methods to manipulate the data */
uint32_t Device_Object_Instance_Number(
    void)
{
    return Object_Instance_Number;
}

bool Device_Set_Object_Instance_Number(
    uint32_t object_id)
{
    bool status = true; /* return value */

    if (object_id <= BACNET_MAX_INSTANCE) {
        Object_Instance_Number = object_id;
        eeprom_bytes_write(NV_EEPROM_DEVICE_0,
            (uint8_t *) & Object_Instance_Number,
            sizeof(Object_Instance_Number));
    } else
        status = false;

    return status;
}

bool Device_Valid_Object_Instance_Number(
    uint32_t object_id)
{
    /* BACnet allows for a wildcard instance number */
    return ((Object_Instance_Number == object_id) ||
        (object_id == BACNET_MAX_INSTANCE));
}

BACNET_DEVICE_STATUS Device_System_Status(
    void)
{
    return System_Status;
}

int Device_Set_System_Status(
    BACNET_DEVICE_STATUS status,
    bool local)
{
    /*return value - 0 = ok, -1 = bad value, -2 = not allowed */
    int result = -1;

    if (status < MAX_DEVICE_STATUS) {
        System_Status = status;
        result = 0;
    }

    return result;
}

uint16_t Device_Vendor_Identifier(
    void)
{
    return BACNET_VENDOR_ID;
}

BACNET_SEGMENTATION Device_Segmentation_Supported(
    void)
{
    return SEGMENTATION_NONE;
}

uint32_t Device_Database_Revision(
    void)
{
    return 0;
}

/* Since many network clients depend on the object list */
/* for discovery, it must be consistent! */
unsigned Device_Object_List_Count(
    void)
{
    unsigned count = 0; /* number of objects */
    struct object_functions *pObject = NULL;

    /* initialize the default return values */
    pObject = &Object_Table[0];
    while (pObject->Object_Type < MAX_BACNET_OBJECT_TYPE) {
        if (pObject->Object_Count) {
            count += pObject->Object_Count();
        }
        pObject++;
    }

    return count;
}

bool Device_Object_List_Identifier(
    unsigned array_index,
    int *object_type,
    uint32_t * instance)
{
    bool status = false;
    unsigned count = 0;
    unsigned object_index = 0;
    struct object_functions *pObject = NULL;

    /* array index zero is length - so invalid */
    if (array_index == 0) {
        return status;
    }
    object_index = array_index - 1;
    /* initialize the default return values */
    pObject = &Object_Table[0];
    while (pObject->Object_Type < MAX_BACNET_OBJECT_TYPE) {
        if (pObject->Object_Count && pObject->Object_Index_To_Instance) {
            object_index -= count;
            count = pObject->Object_Count();
            if (object_index < count) {
                *object_type = pObject->Object_Type;
                *instance = pObject->Object_Index_To_Instance(object_index);
                status = true;
                break;
            }
        }
        pObject++;
    }

    return status;
}

bool Device_Valid_Object_Name(
    const char *object_name,
    int *object_type,
    uint32_t * object_instance)
{
    bool found = false;
    int type = 0;
    uint32_t instance;
    unsigned max_objects = 0, i = 0;
    bool check_id = false;
    char *name = NULL;

    max_objects = Device_Object_List_Count();
    for (i = 0; i < max_objects; i++) {
        check_id = Device_Object_List_Identifier(i, &type, &instance);
        if (check_id) {
            name = Device_Valid_Object_Id(type, instance);
            if (strcmp(name, object_name) == 0) {
                found = true;
                if (object_type) {
                    *object_type = type;
                }
                if (object_instance) {
                    *object_instance = instance;
                }
                break;
            }
        }
    }

    return found;
}

/* returns the name or NULL if not found */
char *Device_Valid_Object_Id(
    int object_type,
    uint32_t object_instance)
{
    char *name = NULL;  /* return value */
    struct object_functions *pObject = NULL;

    pObject = Device_Objects_Find_Functions(object_type);
    if ((pObject) && (pObject->Object_Name)) {
        name = pObject->Object_Name(object_instance);
    }

    return name;
}

/* return the length of the apdu encoded or -1 for error */
int Device_Read_Property_Local(
    BACNET_READ_PROPERTY_DATA * rpdata)
{
    int apdu_len = 0;   /* return value */
    int len = 0;        /* apdu len intermediate value */
    BACNET_BIT_STRING bit_string;
    BACNET_CHARACTER_STRING char_string;
    unsigned i = 0;
    int object_type = 0;
    uint32_t instance = 0;
    unsigned count = 0;
    uint8_t *apdu = NULL;
    struct object_functions *pObject = NULL;

    if ((rpdata->application_data == NULL) ||
        (rpdata->application_data_len == 0)) {
        return 0;
    }
    apdu = rpdata->application_data;
    switch (rpdata->object_property) {
        case PROP_DESCRIPTION:
            characterstring_init_ansi(&char_string, "BACnet Development Kit");
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_SYSTEM_STATUS:
            apdu_len =
                encode_application_enumerated(&apdu[0],
                Device_System_Status());
            break;
        case PROP_VENDOR_NAME:
            characterstring_init_ansi(&char_string, BACNET_VENDOR_NAME);
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_VENDOR_IDENTIFIER:
            apdu_len = encode_application_unsigned(&apdu[0], BACNET_VENDOR_ID);
            break;
        case PROP_MODEL_NAME:
            characterstring_init_ansi(&char_string, "bdk-atxx4-mstp");
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_FIRMWARE_REVISION:
            characterstring_init_ansi(&char_string, BACnet_Version);
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_APPLICATION_SOFTWARE_VERSION:
            characterstring_init_ansi(&char_string, "1.0");
            apdu_len =
                encode_application_character_string(&apdu[0], &char_string);
            break;
        case PROP_PROTOCOL_VERSION:
            apdu_len =
                encode_application_unsigned(&apdu[0], BACNET_PROTOCOL_VERSION);
            break;
        case PROP_PROTOCOL_REVISION:
            apdu_len =
                encode_application_unsigned(&apdu[0],
                BACNET_PROTOCOL_REVISION);
            break;
        case PROP_PROTOCOL_SERVICES_SUPPORTED:
            /* Note: list of services that are executed, not initiated. */
            bitstring_init(&bit_string);
            for (i = 0; i < MAX_BACNET_SERVICES_SUPPORTED; i++) {
                /* automatic lookup based on handlers set */
                bitstring_set_bit(&bit_string, (uint8_t) i,
                    apdu_service_supported(i));
            }
            apdu_len = encode_application_bitstring(&apdu[0], &bit_string);
            break;
        case PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED:
            /* Note: this is the list of objects that can be in this device,
               not a list of objects that this device can access */
            bitstring_init(&bit_string);
            for (i = 0; i < MAX_ASHRAE_OBJECT_TYPE; i++) {
                /* FIXME: if ReadProperty used an array of Functions... */
                /* initialize all the object types to not-supported */
                bitstring_set_bit(&bit_string, (uint8_t) i, false);
            }
            /* set the object types with objects to supported */
            i = 0;
            pObject = &Object_Table[i];
            while (pObject->Object_Type < MAX_BACNET_OBJECT_TYPE) {
                if ((pObject->Object_Count) && (pObject->Object_Count() > 0)) {
                    bitstring_set_bit(&bit_string, pObject->Object_Type, true);
                }
                pObject++;
            }
            apdu_len = encode_application_bitstring(&apdu[0], &bit_string);
            break;
        case PROP_OBJECT_LIST:
            count = Device_Object_List_Count();
            /* Array element zero is the number of objects in the list */
            if (rpdata->array_index == 0)
                apdu_len = encode_application_unsigned(&apdu[0], count);
            /* if no index was specified, then try to encode the entire list */
            /* into one packet.  Note that more than likely you will have */
            /* to return an error if the number of encoded objects exceeds */
            /* your maximum APDU size. */
            else if (rpdata->array_index == BACNET_ARRAY_ALL) {
                for (i = 1; i <= count; i++) {
                    if (Device_Object_List_Identifier(i, &object_type,
                            &instance)) {
                        len =
                            encode_application_object_id(&apdu[apdu_len],
                            object_type, instance);
                        apdu_len += len;
                        /* assume next one is the same size as this one */
                        /* can we all fit into the APDU? */
                        if ((apdu_len + len) >= MAX_APDU) {
                            rpdata->error_class = ERROR_CLASS_SERVICES;
                            rpdata->error_code =
                                ERROR_CODE_NO_SPACE_FOR_OBJECT;
                            apdu_len = BACNET_STATUS_ERROR;
                            break;
                        }
                    } else {
                        /* error: internal error? */
                        rpdata->error_class = ERROR_CLASS_SERVICES;
                        rpdata->error_code = ERROR_CODE_OTHER;
                        apdu_len = BACNET_STATUS_ERROR;
                        break;
                    }
                }
            } else {
                if (Device_Object_List_Identifier(rpdata->array_index,
                        &object_type, &instance))
                    apdu_len =
                        encode_application_object_id(&apdu[0], object_type,
                        instance);
                else {
                    rpdata->error_class = ERROR_CLASS_PROPERTY;
                    rpdata->error_code = ERROR_CODE_INVALID_ARRAY_INDEX;
                    apdu_len = BACNET_STATUS_ERROR;
                }
            }
            break;
        case PROP_MAX_APDU_LENGTH_ACCEPTED:
            apdu_len = encode_application_unsigned(&apdu[0], MAX_APDU);
            break;
        case PROP_SEGMENTATION_SUPPORTED:
            apdu_len =
                encode_application_enumerated(&apdu[0],
                Device_Segmentation_Supported());
            break;
        case PROP_APDU_TIMEOUT:
            apdu_len = encode_application_unsigned(&apdu[0], apdu_timeout());
            break;
        case PROP_NUMBER_OF_APDU_RETRIES:
            apdu_len = encode_application_unsigned(&apdu[0], apdu_retries());
            break;
        case PROP_DEVICE_ADDRESS_BINDING:
            /* FIXME: encode the list here, if it exists */
            break;
        case PROP_DATABASE_REVISION:
            apdu_len =
                encode_application_unsigned(&apdu[0],
                Device_Database_Revision());
            break;
        case PROP_MAX_INFO_FRAMES:
            apdu_len =
                encode_application_unsigned(&apdu[0],
                dlmstp_max_info_frames());
            break;
        case PROP_MAX_MASTER:
            apdu_len =
                encode_application_unsigned(&apdu[0], dlmstp_max_master());
            break;
        case 512:
            apdu_len = encode_application_unsigned(&apdu[0], stack_size());
            break;
        case 513:
            apdu_len = encode_application_unsigned(&apdu[0], stack_unused());
            break;
        case 9600:
            apdu_len =
                encode_application_unsigned(&apdu[0], rs485_baud_rate());
            break;
        default:
            rpdata->error_class = ERROR_CLASS_PROPERTY;
            rpdata->error_code = ERROR_CODE_UNKNOWN_PROPERTY;
            apdu_len = BACNET_STATUS_ERROR;
            break;
    }
    /*  only array properties can have array options */
    if ((apdu_len >= 0) && (rpdata->object_property != PROP_OBJECT_LIST) &&
        (rpdata->array_index != BACNET_ARRAY_ALL)) {
        rpdata->error_class = ERROR_CLASS_PROPERTY;
        rpdata->error_code = ERROR_CODE_PROPERTY_IS_NOT_AN_ARRAY;
        apdu_len = BACNET_STATUS_ERROR;
    }

    return apdu_len;
}

bool Device_Write_Property_Local(
    BACNET_WRITE_PROPERTY_DATA * wp_data)
{
    bool status = false;        /* return value - false=error */
    int len = 0;
    BACNET_APPLICATION_DATA_VALUE value;

    /* decode the some of the request */
    len =
        bacapp_decode_application_data(wp_data->application_data,
        wp_data->application_data_len, &value);
    /* FIXME: len < application_data_len: more data? */
    /* FIXME: len == 0: unable to decode? */
    switch (wp_data->object_property) {
        case PROP_OBJECT_IDENTIFIER:
            if (value.tag == BACNET_APPLICATION_TAG_OBJECT_ID) {
                if ((value.type.Object_Id.type == OBJECT_DEVICE) &&
                    (Device_Set_Object_Instance_Number(value.type.Object_Id.
                            instance))) {
                    /* we could send an I-Am broadcast to let the world know */
                    status = true;
                } else {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
                }
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_INVALID_DATA_TYPE;
            }
            break;
        case PROP_MAX_INFO_FRAMES:
            if (value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                if (value.type.Unsigned_Int <= 255) {
                    dlmstp_set_max_info_frames(value.type.Unsigned_Int);
                    status = true;
                } else {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
                }
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_INVALID_DATA_TYPE;
            }
            break;
        case PROP_MAX_MASTER:
            if (value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                if ((value.type.Unsigned_Int > 0) &&
                    (value.type.Unsigned_Int <= 127)) {
                    dlmstp_set_max_master(value.type.Unsigned_Int);
                    status = true;
                } else {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
                }
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_INVALID_DATA_TYPE;
            }
            break;
        case PROP_OBJECT_NAME:
            if (value.tag == BACNET_APPLICATION_TAG_CHARACTER_STRING) {
                size_t length =
                    characterstring_length(&value.type.Character_String);
                if (length < 1) {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
                } else if (length < NV_EEPROM_DEVICE_NAME_SIZE) {
                    uint8_t encoding =
                        characterstring_encoding(&value.type.Character_String);
                    if (encoding < MAX_CHARACTER_STRING_ENCODING) {
                        char *pCharString;
                        uint8_t small_length;
                        eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_ENCODING,
                            &encoding, 1);
                        small_length = length;
                        eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_LENGTH,
                            &small_length, 1);
                        pCharString =
                            characterstring_value(&value.type.
                            Character_String);
                        eeprom_bytes_write(NV_EEPROM_DEVICE_NAME_0,
                            (uint8_t *) pCharString, length);
                        status = true;
                    } else {
                        wp_data->error_class = ERROR_CLASS_PROPERTY;
                        wp_data->error_code =
                            ERROR_CODE_CHARACTER_SET_NOT_SUPPORTED;
                    }
                } else {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code =
                        ERROR_CODE_NO_SPACE_TO_WRITE_PROPERTY;
                }
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_INVALID_DATA_TYPE;
            }
            break;
        case 9600:
            if (value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                if ((value.type.Unsigned_Int <= 115200) &&
                    (rs485_baud_rate_set(value.type.Unsigned_Int))) {
                    status = true;
                } else {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
                }
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code = ERROR_CODE_INVALID_DATA_TYPE;
            }
            break;
        default:
            wp_data->error_class = ERROR_CLASS_PROPERTY;
            wp_data->error_code = ERROR_CODE_WRITE_ACCESS_DENIED;
            break;
    }

    return status;
}
