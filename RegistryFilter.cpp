#include "RegistryFilter.h"

LARGE_INTEGER RegistryBlockingFilter::RegistryFilterCookie;

/**
	Initializes the necessary components of the registry filter.
	@param DriverObject - The object of the driver necessary for mini-filter initialization.
	@param InitializeStatus - Status of initialization.
*/
RegistryBlockingFilter::RegistryBlockingFilter(
	_In_ PDRIVER_OBJECT DriverObject,
	_Out_ NTSTATUS* InitializeStatus
	)
{
	UNICODE_STRING filterAltitude;

	RegistryBlockingFilter::RegistryStringFilters = new (NonPagedPool, STRING_REGISTRY_FILTERS_TAG) StringFilters();
	if (RegistryBlockingFilter::RegistryStringFilters == NULL)
	{
		DBGPRINT("RegistryBlockingFilter!RegistryBlockingFilter: Failed to allocate memory for string filters.");
		*InitializeStatus = STATUS_NO_MEMORY;
		return;
	}

	//
	// Put our altitude into a UNICODE_STRING.
	//
	RtlInitUnicodeString(&filterAltitude, FILTER_ALTITUDE);

	//
	// Register our registry callback.
	//
	*InitializeStatus = CmRegisterCallbackEx(RCAST<PEX_CALLBACK_FUNCTION>(RegistryBlockingFilter::RegistryCallback), &filterAltitude, DriverObject, NULL, &RegistryFilterCookie, NULL);
}

/**
	Free data members that were dynamically allocated.
*/
RegistryBlockingFilter::~RegistryBlockingFilter()
{
	//
	// Remove the registry callback.
	//
	CmUnRegisterCallback(this->RegistryFilterCookie);

	//
	// Make sure to deconstruct the class.
	//
	RegistryBlockingFilter::RegistryStringFilters->~StringFilters();
	ExFreePoolWithTag(RegistryBlockingFilter::RegistryStringFilters, STRING_REGISTRY_FILTERS_TAG);
}

/**
	Return the string filters used in the registry filter.
	@return String filters for registry operations.
*/
PSTRING_FILTERS RegistryBlockingFilter::GetStringFilters()
{
	return RegistryBlockingFilter::RegistryStringFilters;
}

PSTRING_FILTERS RegistryBlockingFilter::RegistryStringFilters;

/**
	Function that decides whether or not to block a registry operation.
*/
BOOLEAN
RegistryBlockingFilter::BlockRegistryOperation (
	_In_ PVOID KeyObject,
	_In_ PUNICODE_STRING ValueName,
	_In_ ULONG OperationFlag
	)
{
	BOOLEAN blockOperation;
	NTSTATUS internalStatus;
	HANDLE keyHandle;
	PKEY_NAME_INFORMATION pKeyNameInformation;
	ULONG returnLength;
	ULONG fullKeyValueLength;
	PWCHAR fullKeyValueName;

	blockOperation = FALSE;
	keyHandle = NULL;
	returnLength = NULL;
	pKeyNameInformation = NULL;
	fullKeyValueName = NULL;

	if (ValueName == NULL || ValueName->Length == 0 || ValueName->Buffer == NULL)
	{
		DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: ValueName is NULL.");
		goto Exit;
	}

	//
	// There can be some wonky exceptions with weird input,
	// just in case we don't handle something is a simple
	// catch all.
	//
	__try {
		//
		// Open the registry key.
		//
		internalStatus = ObOpenObjectByPointer(KeyObject, OBJ_KERNEL_HANDLE, NULL, GENERIC_ALL, *CmKeyObjectType, KernelMode, &keyHandle);
		if (NT_SUCCESS(internalStatus) == FALSE)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to open a handle to a key object with status 0x%X.", internalStatus);
			goto Exit;
		}

		ZwQueryKey(keyHandle, KeyNameInformation, NULL, 0, &returnLength);
		if (returnLength == 0)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to determine size of key name.");
			goto Exit;
		}

		returnLength += 1; // For null terminator.
		pKeyNameInformation = RCAST<PKEY_NAME_INFORMATION>(ExAllocatePoolWithTag(PagedPool, returnLength, REGISTRY_KEY_NAME_TAG));
		if (pKeyNameInformation == NULL)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to allocate memory for key name with size 0x%X.", returnLength);
			goto Exit;
		}

		//
		// Query the name information of the key to retrieve its name.
		//
		internalStatus = ZwQueryKey(keyHandle, KeyNameInformation, pKeyNameInformation, returnLength, &returnLength);
		if (NT_SUCCESS(internalStatus) == FALSE)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to query name of key object with status 0x%X.", internalStatus);
			goto Exit;
		}

		//
		// Append a null terminator.
		//
		*RCAST<USHORT*>(RCAST<ULONG64>(&pKeyNameInformation->Name) + pKeyNameInformation->NameLength) = 0;

		//
		// Allocate space for key name, a backslash, the value name, and the null-terminator.
		//
		fullKeyValueLength = pKeyNameInformation->NameLength + 2 + ValueName->Length + 1000;
		fullKeyValueName = RCAST<PWCHAR>(ExAllocatePoolWithTag(PagedPool, fullKeyValueLength, REGISTRY_KEY_NAME_TAG));
		if (fullKeyValueName == NULL)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to allocate memory for full key/value name with size 0x%X.", fullKeyValueLength);
			goto Exit;
		}

		//
		// Copy the key name.
		//
		internalStatus = RtlStringCbCopyW(fullKeyValueName, fullKeyValueLength, RCAST<PCWSTR>(&pKeyNameInformation->Name));
		if (NT_SUCCESS(internalStatus) == FALSE)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to copy key name with status 0x%X.", internalStatus);
			goto Exit;
		}

		//
		// Concatenate the backslash.
		//
		internalStatus = RtlStringCbCatW(fullKeyValueName, fullKeyValueLength, L"\\");
		if (NT_SUCCESS(internalStatus) == FALSE)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to concatenate backslash with status 0x%X.", internalStatus);
			goto Exit;
		}

		//
		// Concatenate the value name.
		//
		internalStatus = RtlStringCbCatW(fullKeyValueName, fullKeyValueLength, ValueName->Buffer);
		if (NT_SUCCESS(internalStatus) == FALSE)
		{
			DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Failed to concatenate value name with status 0x%X.", internalStatus);
			goto Exit;
		}

		blockOperation = RegistryBlockingFilter::RegistryStringFilters->MatchesFilter(fullKeyValueName, OperationFlag);

		//DBGPRINT("RegistryBlockingFilter!BlockRegistryOperation: Full name: %S.", fullKeyValueName);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{}

Exit:
	if (fullKeyValueName)
	{
		ExFreePoolWithTag(fullKeyValueName, REGISTRY_KEY_NAME_TAG);
	}
	if (pKeyNameInformation)
	{
		ExFreePoolWithTag(pKeyNameInformation, REGISTRY_KEY_NAME_TAG);
	}
	if (keyHandle)
	{
		ZwClose(keyHandle);
	}
	return blockOperation;
}


NTSTATUS RegistryBlockingFilter::RegistryCallback (
	_In_ PVOID CallbackContext,
	_In_ REG_NOTIFY_CLASS OperationClass, 
	_In_ PVOID Argument2
	)
{
	UNREFERENCED_PARAMETER(CallbackContext);
	NTSTATUS returnStatus;
	PREG_SET_VALUE_KEY_INFORMATION setValueInformation;
	PREG_DELETE_VALUE_KEY_INFORMATION deleteValueInformation;

	returnStatus = STATUS_SUCCESS;

	switch (OperationClass)
	{
	case RegNtPreSetValueKey:
		setValueInformation = RCAST<PREG_SET_VALUE_KEY_INFORMATION>(Argument2);
		if (BlockRegistryOperation(setValueInformation->Object, setValueInformation->ValueName, FILTER_FLAG_WRITE))
		{
			DBGPRINT("RegistryBlockingFilter!RegistryCallback: Detected RegNtPreSetValueKey of %wZ. Prevented set!", setValueInformation->ValueName);
			returnStatus = STATUS_ACCESS_DENIED;
		}	
		break;
	case RegNtPreDeleteValueKey:
		deleteValueInformation = RCAST<PREG_DELETE_VALUE_KEY_INFORMATION>(Argument2);
		if (BlockRegistryOperation(deleteValueInformation->Object, deleteValueInformation->ValueName, FILTER_FLAG_DELETE))
		{
			DBGPRINT("RegistryBlockingFilter!RegistryCallback: Detected RegNtPreDeleteValueKey of %wZ. Prevented rewrite!", deleteValueInformation->ValueName);
			returnStatus = STATUS_ACCESS_DENIED;
		}
		break;
	}


	return returnStatus;
}