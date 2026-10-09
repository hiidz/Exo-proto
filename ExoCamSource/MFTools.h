#pragma once

void TraceMFAttributes(IUnknown* unknown, PCWSTR prefix);
std::wstring PKSIDENTIFIER_ToString(PKSIDENTIFIER id, ULONG length);

// Pass-through IMFAttributes implementation. It forwards everything to a real
// attribute store; the reason it exists is to trace what Media Foundation reads
// from and writes to the activator, the source and the stream.
//
// Every method here traces, and it must stay behind LOG_DETAIL: this is
// per-property-access tracing (GetItemByIndex runs in a loop over every
// attribute an object holds) against a sink that writes and flushes
// synchronously. Unconditional, it would make the log unreadable and put file
// I/O on a COM hot path.
template <class IFACE = IMFAttributes>
struct CBaseAttributes : public IFACE
{
protected:
	wil::com_ptr_nothrow<IMFAttributes> _attributes;
	std::wstring _trace;

	CBaseAttributes() :
		_trace(L"Atts")
	{
		THROW_IF_FAILED(MFCreateAttributes(&_attributes, 0));
	}

	void SetBaseAttributesTraceName(std::wstring trace)
	{
		_trace = trace;
	}

public:
	STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* value)
	{
		RETURN_HR_IF(E_INVALIDARG, !value);
		assert(_attributes);
		auto hr = _attributes->GetItem(guidKey, value);
		LOG_DETAIL(L"%s:GetItem '%s' value:%s", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), PROPVARIANT_ToString(*value).c_str());
		return hr;
	}

	STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType)
	{
		RETURN_HR_IF(E_INVALIDARG, !pType);
		*pType = (MF_ATTRIBUTE_TYPE)0;
		assert(_attributes);
		auto hr = _attributes->GetItemType(guidKey, pType);
		LOG_DETAIL(L"%s:GetItemType '%s' type:%s hr:0x%08X", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), MF_ATTRIBUTE_TYPE_ToString(*pType).c_str(), hr);
		return hr;
	}

	STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pbResult);
		assert(_attributes);
		LOG_DETAIL(L"%s:CompareItem '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->CompareItem(guidKey, Value, pbResult);
	}

	STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pTheirs || !pbResult);
		assert(_attributes);
		LOG_DETAIL(L"%s:Compare", _trace.c_str());
		return _attributes->Compare(pTheirs, MatchType, pbResult);
	}

	STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		auto hr = _attributes->GetUINT32(guidKey, punValue);
		LOG_DETAIL(L"%s:GetUINT32 '%s' hr:0x%08X value:%u/0x%08X", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), hr, *punValue, *punValue);
		return hr;
	}

	STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		auto hr = _attributes->GetUINT64(guidKey, punValue);
		LOG_DETAIL(L"%s:GetUINT64 '%s' hr:0x%08X value:%I64i/0x%016X", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), hr, *punValue, *punValue);
		return hr;
	}

	STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pfValue);
		*pfValue = 0;
		assert(_attributes);
		auto hr = _attributes->GetDouble(guidKey, pfValue);
		LOG_DETAIL(L"%s:GetDouble '%s' hr:0x%08X", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), hr);
		return hr;
	}

	STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pguidValue);
		ZeroMemory(pguidValue, 16);
		assert(_attributes);
		auto hr = _attributes->GetGUID(guidKey, pguidValue);
		LOG_DETAIL(L"%s:GetGUID '%s' hr:0x%08X value:'%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), hr, GUID_ToStringW(*pguidValue).c_str());
		return hr;
	}

	STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcchLength);
		*pcchLength = 0;
		assert(_attributes);
		auto hr = _attributes->GetStringLength(guidKey, pcchLength);
		LOG_DETAIL(L"%s:GetStringLength '%s' len:%u", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), *pcchLength);
		return hr;
	}

	STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:GetString '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength);
	}

	STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppwszValue || !pcchLength);
		*ppwszValue = 0;
		*pcchLength = 0;
		assert(_attributes);
		auto hr = _attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength);
		LOG_DETAIL(L"%s:GetAllocatedString hr:0x%08X '%s' len:%u value:'%s'", _trace.c_str(), hr, GUID_ToStringW(guidKey).c_str(), *pcchLength, ppwszValue);
		return hr;
	}

	STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcbBlobSize);
		assert(_attributes);
		LOG_DETAIL(L"%s:GetBlobSize '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->GetBlobSize(guidKey, pcbBlobSize);
	}

	STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:GetBlob '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize);
	}

	STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppBuf || !pcbSize);
		assert(_attributes);
		LOG_DETAIL(L"%s:GetAllocatedBlob '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize);
	}

	STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppv);
		assert(_attributes);
		auto hr = _attributes->GetUnknown(guidKey, riid, ppv);
		LOG_DETAIL(L"%s:GetUnknown hr:0x%08X '%s' riid:'%s' %p", _trace.c_str(), hr, GUID_ToStringW(guidKey).c_str(), GUID_ToStringW(riid).c_str(), *ppv);
		return hr;
	}

	STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT value)
	{
		assert(_attributes);
		auto v = PROPVARIANT_ToString(value);
		LOG_DETAIL(L"%s:SetItem '%s' value:%s", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), v.c_str());
		return _attributes->SetItem(guidKey, value);
	}

	STDMETHODIMP DeleteItem(REFGUID guidKey)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:DeleteItem '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->DeleteItem(guidKey);
	}

	STDMETHODIMP DeleteAllItems()
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:DeleteAllItems", _trace.c_str());
		return _attributes->DeleteAllItems();
	}

	STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetUINT32 '%s' value:%u", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), value);
		return _attributes->SetUINT32(guidKey, value);
	}

	STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetUINT64 '%s' value:%I64i", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), value);
		return _attributes->SetUINT64(guidKey, value);
	}

	STDMETHODIMP SetDouble(REFGUID guidKey, double value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetDouble '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->SetDouble(guidKey, value);
	}

	STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetGUID '%s' value:'%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), GUID_ToStringW(value).c_str());
		return _attributes->SetGUID(guidKey, value);
	}

	STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetString '%s' value:'%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), value);
		return _attributes->SetString(guidKey, value);
	}

	STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetBlob '%s'", _trace.c_str(), GUID_ToStringW(guidKey).c_str());
		return _attributes->SetBlob(guidKey, pBuf, cbBufSize);
	}

	STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* value)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:SetUnknown '%s' value:%p", _trace.c_str(), GUID_ToStringW(guidKey).c_str(), value);
		return _attributes->SetUnknown(guidKey, value);
	}

	STDMETHODIMP LockStore()
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:LockStore", _trace.c_str());
		return _attributes->LockStore();
	}

	STDMETHODIMP UnlockStore()
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:UnlockStore", _trace.c_str());
		return _attributes->UnlockStore();
	}

	STDMETHODIMP GetCount(UINT32* pcItems)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcItems);
		assert(_attributes);
		auto hr = _attributes->GetCount(pcItems);
		LOG_DETAIL(L"%s:GetCount %u hr:0x%08X", _trace.c_str(), *pcItems, hr);
		return hr;
	}

	STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue)
	{
		assert(_attributes);
		LOG_DETAIL(L"%s:GetItemByIndex %u", _trace.c_str(), unIndex);
		return _attributes->GetItemByIndex(unIndex, pguidKey, pValue);
	}

	STDMETHODIMP CopyAllItems(IMFAttributes* pDest)
	{
		RETURN_HR_IF(E_INVALIDARG, !pDest);
		assert(_attributes);
		LOG_DETAIL(L"%s:CopyAllItems", _trace.c_str());
		return _attributes->CopyAllItems(pDest);
	}

	HRESULT SerializeToStream(DWORD dwOptions, IStream* pStm)
	{
		RETURN_HR_IF(E_INVALIDARG, !pStm);
		assert(_attributes);
		return MFSerializeAttributesToStream(_attributes.get(), dwOptions, pStm);
	}

	HRESULT DeserializeFromStream(DWORD dwOptions, IStream* pStm)
	{
		RETURN_HR_IF(E_INVALIDARG, !pStm);
		assert(_attributes);
		return MFDeserializeAttributesFromStream(_attributes.get(), dwOptions, pStm);
	}

	HRESULT SerializeToBlob(UINT8** buffer, UINT32* size)
	{
		RETURN_HR_IF(E_INVALIDARG, !_attributes || !size);
		assert(_attributes);

		*buffer = NULL;
		*size = 0;

		UINT32 cbSize = 0;
		RETURN_IF_FAILED(MFGetAttributesAsBlobSize(_attributes.get(), &cbSize));

		auto pBuffer = (BYTE*)CoTaskMemAlloc(cbSize);
		RETURN_IF_NULL_ALLOC(pBuffer);

		auto hr = MFGetAttributesAsBlob(_attributes.get(), pBuffer, cbSize);
		if (SUCCEEDED(hr))
		{
			*buffer = pBuffer;
			*size = cbSize;
		}
		else
		{
			CoTaskMemFree(pBuffer);
		}
		return hr;
	}

	HRESULT DeserializeFromBlob(const UINT8* buffer, UINT size)
	{
		RETURN_HR_IF(E_INVALIDARG, !buffer || !size);
		assert(_attributes);
		return MFInitAttributesFromBlob(_attributes.get(), buffer, size);
	}

	HRESULT GetRatio(REFGUID guidKey, UINT32* pnNumerator, UINT32* punDenominator)
	{
		RETURN_HR_IF(E_INVALIDARG, !pnNumerator || !punDenominator);
		assert(_attributes);
		return MFGetAttributeRatio(_attributes.get(), guidKey, pnNumerator, punDenominator);
	}

	HRESULT SetRatio(REFGUID guidKey, UINT32 unNumerator, UINT32 unDenominator)
	{
		assert(_attributes);
		return MFSetAttributeRatio(_attributes.get(), guidKey, unNumerator, unDenominator);
	}

	HRESULT GetSize(REFGUID guidKey, UINT32* punWidth, UINT32* punHeight)
	{
		RETURN_HR_IF(E_INVALIDARG, !punWidth || !punWidth);
		assert(_attributes);
		return MFGetAttributeSize(_attributes.get(), guidKey, punWidth, punHeight);
	}

	HRESULT SetSize(REFGUID guidKey, UINT32 unWidth, UINT32 unHeight)
	{
		assert(_attributes);
		return MFSetAttributeSize(_attributes.get(), guidKey, unWidth, unHeight);
	}
};