// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

/*
 * Wraps a treeview control. Specifically handles
 * adding directories to it and selecting directories.
 * Each non-network drive in the system is also
 * monitored for changes.
 *
 * Notes:
 *  - All items are sorted alphabetically, except for:
 *     - Items on the desktop
 *     - Items in My Computer
 */

#include "stdafx.h"
#include "ShellTreeView.h"
#include "Config.h"
#include "CoreInterface.h"
#include "DarkModeHelper.h"
#include "TabContainer.h"
#include "../Helper/CachedIcons.h"
#include "../Helper/ClipboardHelper.h"
#include "../Helper/Controls.h"
#include "../Helper/DragDropHelper.h"
#include "../Helper/DriveInfo.h"
#include "../Helper/FileActionHandler.h"
#include "../Helper/FileOperations.h"
#include "../Helper/Helper.h"
#include "../Helper/Macros.h"
#include "../Helper/ShellHelper.h"
#include <wil/common.h>
#include <propkey.h>

int CALLBACK CompareItemsStub(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);

ShellTreeView::ShellTreeView(HWND hParent, CoreInterface *coreInterface, TabContainer *tabContainer,
	FileActionHandler *fileActionHandler, CachedIcons *cachedIcons) :
	ShellDropTargetWindow(CreateTreeView(hParent)),
	m_hTreeView(GetHWND()),
	m_config(coreInterface->GetConfig()),
	m_tabContainer(tabContainer),
	m_fileActionHandler(fileActionHandler),
	m_cachedIcons(cachedIcons),
	m_itemIDCounter(0),
	m_iconThreadPool(1, std::bind(CoInitializeEx, nullptr, COINIT_APARTMENTTHREADED),
		CoUninitialize),
	m_iconResultIDCounter(0),
	m_subfoldersThreadPool(1, std::bind(CoInitializeEx, nullptr, COINIT_APARTMENTTHREADED),
		CoUninitialize),
	m_subfoldersResultIDCounter(0),
	m_cutItem(nullptr),
	m_dropExpandItem(nullptr)
{
	auto &darkModeHelper = DarkModeHelper::GetInstance();

	if (DarkModeHelper::GetInstance().IsDarkModeEnabled())
	{
		darkModeHelper.AllowDarkModeForWindow(m_hTreeView, true);

		TreeView_SetBkColor(m_hTreeView, TREE_VIEW_DARK_MODE_BACKGROUND_COLOR);
		TreeView_SetTextColor(m_hTreeView, DarkModeHelper::TEXT_COLOR);

		InvalidateRect(m_hTreeView, nullptr, TRUE);

		HWND tooltips = TreeView_GetToolTips(m_hTreeView);
		darkModeHelper.AllowDarkModeForWindow(tooltips, true);
		SetWindowTheme(tooltips, L"Explorer", nullptr);
	}

	SetWindowTheme(m_hTreeView, L"Explorer", nullptr);

	m_windowSubclasses.push_back(std::make_unique<WindowSubclassWrapper>(m_hTreeView,
		TreeViewProcStub, reinterpret_cast<DWORD_PTR>(this)));
	m_windowSubclasses.push_back(std::make_unique<WindowSubclassWrapper>(hParent, ParentWndProcStub,
		reinterpret_cast<DWORD_PTR>(this)));

	m_iFolderIcon = GetDefaultFolderIconIndex();

	m_bDragCancelled = FALSE;
	m_bDragAllowed = FALSE;
	m_bShowHidden = TRUE;

	AddRoot();

	m_getDragImageMessage = RegisterWindowMessage(DI_GETDRAGIMAGE);

	AddClipboardFormatListener(m_hTreeView);

	m_connections.push_back(coreInterface->AddApplicationShuttingDownObserver(
		std::bind_front(&ShellTreeView::OnApplicationShuttingDown, this)));
}

HWND ShellTreeView::CreateTreeView(HWND parent)
{
	return ::CreateTreeView(parent,
		WS_CHILD | WS_VISIBLE | TVS_SHOWSELALWAYS | TVS_HASBUTTONS | TVS_EDITLABELS | TVS_HASLINES
			| TVS_TRACKSELECT);
}

ShellTreeView::~ShellTreeView()
{
	m_iconThreadPool.clear_queue();
}

void ShellTreeView::OnApplicationShuttingDown()
{
	if (m_clipboardDataObject && OleIsCurrentClipboard(m_clipboardDataObject.get()) == S_OK)
	{
		OleFlushClipboard();
	}
}

LRESULT CALLBACK ShellTreeView::TreeViewProcStub(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);

	auto *shellTreeView = reinterpret_cast<ShellTreeView *>(dwRefData);

	return shellTreeView->TreeViewProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ShellTreeView::TreeViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (m_getDragImageMessage != 0 && msg == m_getDragImageMessage)
	{
		return FALSE;
	}

	switch (msg)
	{
	case WM_TIMER:
		if (wParam == PROCESS_SHELL_CHANGES_TIMER_ID)
		{
			OnProcessShellChangeNotifications();
		}
		else if (wParam == DROP_EXPAND_TIMER_ID)
		{
			OnDropExpandTimer();
		}
		break;

	case WM_RBUTTONDOWN:
		if ((wParam & MK_RBUTTON) && !(wParam & MK_LBUTTON) && !(wParam & MK_MBUTTON))
		{
			TVHITTESTINFO tvhti;

			tvhti.pt.x = LOWORD(lParam);
			tvhti.pt.y = HIWORD(lParam);

			/* Test to see if the mouse click was
			on an item or not. */
			TreeView_HitTest(m_hTreeView, &tvhti);

			if (!(tvhti.flags & LVHT_NOWHERE))
			{
				m_bDragAllowed = TRUE;
			}
		}
		break;

	case WM_RBUTTONUP:
		m_bDragCancelled = FALSE;
		m_bDragAllowed = FALSE;
		break;

	case WM_MBUTTONDOWN:
	{
		POINT pt;
		POINTSTOPOINT(pt, MAKEPOINTS(lParam));
		OnMiddleButtonDown(&pt);
	}
	break;

	case WM_MBUTTONUP:
	{
		POINT pt;
		POINTSTOPOINT(pt, MAKEPOINTS(lParam));
		OnMiddleButtonUp(&pt, static_cast<UINT>(wParam));
	}
	break;

	case WM_MOUSEMOVE:
	{
		if (!IsWithinDrag() && !m_bDragCancelled && m_bDragAllowed)
		{
			if ((wParam & MK_RBUTTON) && !(wParam & MK_LBUTTON) && !(wParam & MK_MBUTTON))
			{
				TVHITTESTINFO tvhti;
				TVITEM tvItem;
				POINT pt;
				DWORD dwPos;
				HRESULT hr;
				BOOL bRet;

				dwPos = GetMessagePos();
				pt.x = GET_X_LPARAM(dwPos);
				pt.y = GET_Y_LPARAM(dwPos);
				MapWindowPoints(HWND_DESKTOP, m_hTreeView, &pt, 1);

				tvhti.pt = pt;

				/* Test to see if the mouse click was
				on an item or not. */
				TreeView_HitTest(m_hTreeView, &tvhti);

				if (!(tvhti.flags & LVHT_NOWHERE))
				{
					tvItem.mask = TVIF_PARAM | TVIF_HANDLE;
					tvItem.hItem = tvhti.hItem;
					bRet = TreeView_GetItem(m_hTreeView, &tvItem);

					if (bRet)
					{
						hr = OnBeginDrag((int) tvItem.lParam);

						if (hr == DRAGDROP_S_CANCEL)
						{
							m_bDragCancelled = TRUE;
						}
					}
				}
			}
		}
	}
	break;

	case WM_CLIPBOARDUPDATE:
		OnClipboardUpdate();
		return 0;

	case WM_APP_ICON_RESULT_READY:
		ProcessIconResult(static_cast<int>(wParam));
		break;

	case WM_APP_SUBFOLDERS_RESULT_READY:
		ProcessSubfoldersResult(static_cast<int>(wParam));
		break;

	case WM_APP_SHELL_NOTIFY:
		OnShellNotify(wParam, lParam);
		break;

	case WM_DESTROY:
		RemoveClipboardFormatListener(m_hTreeView);
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ShellTreeView::ParentWndProcStub(HWND hwnd, UINT uMsg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);

	auto *treeView = reinterpret_cast<ShellTreeView *>(dwRefData);
	return treeView->ParentWndProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ShellTreeView::ParentWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_NOTIFY:
		if (reinterpret_cast<LPNMHDR>(lParam)->hwndFrom == m_hTreeView)
		{
			switch (reinterpret_cast<LPNMHDR>(lParam)->code)
			{
			case TVN_BEGINDRAG:
			{
				auto *pnmTreeView = reinterpret_cast<NMTREEVIEW *>(lParam);
				OnBeginDrag(static_cast<int>(pnmTreeView->itemNew.lParam));
			}
			break;

			case TVN_GETDISPINFO:
				OnGetDisplayInfo(reinterpret_cast<NMTVDISPINFO *>(lParam));
				break;

			case TVN_ITEMEXPANDING:
				OnItemExpanding(reinterpret_cast<NMTREEVIEW *>(lParam));
				break;

			case TVN_KEYDOWN:
				return OnKeyDown(reinterpret_cast<NMTVKEYDOWN *>(lParam));

			case TVN_ENDLABELEDIT:
				/* TODO: Should return the value from this function. Can't do it
				at the moment, since the treeview looks items up by their label
				when a directory modification event is received (meaning that if
				the label changes, the lookup for the old file name will fail). */
				OnEndLabelEdit(reinterpret_cast<NMTVDISPINFO *>(lParam));
				break;
			}
		}
		break;
	}

	return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

HTREEITEM ShellTreeView::AddRoot()
{
	TreeView_DeleteAllItems(m_hTreeView);

	unique_pidl_absolute pidl;
	HRESULT hr = GetRootPidl(wil::out_param(pidl));

	if (FAILED(hr))
	{
		return nullptr;
	}

	std::wstring desktopDisplayName;
	GetDisplayName(pidl.get(), SHGDN_INFOLDER, desktopDisplayName);

	int itemId = GenerateUniqueItemId();
	m_itemInfoMap[itemId].pidl.reset(ILCloneFull(pidl.get()));

	TVITEMEX tvItem;
	tvItem.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
	tvItem.pszText = desktopDisplayName.data();
	tvItem.iImage = I_IMAGECALLBACK;
	tvItem.iSelectedImage = I_IMAGECALLBACK;
	tvItem.cChildren = 1;
	tvItem.lParam = itemId;

	TVINSERTSTRUCT tvis;
	tvis.hParent = nullptr;
	tvis.hInsertAfter = TVI_LAST;
	tvis.itemex = tvItem;

	auto hDesktop = TreeView_InsertItem(m_hTreeView, &tvis);

	if (hDesktop != nullptr)
	{
		SendMessage(m_hTreeView, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(hDesktop));
	}

	return hDesktop;
}

void ShellTreeView::OnGetDisplayInfo(NMTVDISPINFO *pnmtvdi)
{
	TVITEM *ptvItem = &pnmtvdi->item;

	if (WI_IsFlagSet(ptvItem->mask, TVIF_IMAGE))
	{
		const ItemInfo &itemInfo = m_itemInfoMap.at(static_cast<int>(ptvItem->lParam));
		auto cachedIconIndex = GetCachedIconIndex(itemInfo);

		if (cachedIconIndex)
		{
			ptvItem->iImage = (*cachedIconIndex & 0x0FFF);
			ptvItem->iSelectedImage = (*cachedIconIndex & 0x0FFF);
		}
		else
		{
			ptvItem->iImage = m_iFolderIcon;
			ptvItem->iSelectedImage = m_iFolderIcon;
		}

		QueueIconTask(ptvItem->hItem, static_cast<int>(ptvItem->lParam));
	}

	if (WI_IsFlagSet(ptvItem->mask, TVIF_CHILDREN))
	{
		ptvItem->cChildren = 1;

		QueueSubfoldersTask(ptvItem->hItem);
	}

	ptvItem->mask |= TVIF_DI_SETITEM;
}

std::optional<int> ShellTreeView::GetCachedIconIndex(const ItemInfo &itemInfo)
{
	std::wstring filePath;
	HRESULT hr = GetDisplayName(itemInfo.pidl.get(), SHGDN_FORPARSING, filePath);

	if (FAILED(hr))
	{
		return std::nullopt;
	}

	auto cachedItr = m_cachedIcons->findByPath(filePath);

	if (cachedItr == m_cachedIcons->end())
	{
		return std::nullopt;
	}

	return cachedItr->iconIndex;
}

void ShellTreeView::QueueIconTask(HTREEITEM item, int internalIndex)
{
	const ItemInfo &itemInfo = m_itemInfoMap.at(internalIndex);

	BasicItemInfo basicItemInfo;
	basicItemInfo.pidl.reset(ILCloneFull(itemInfo.pidl.get()));

	int iconResultID = m_iconResultIDCounter++;

	auto result = m_iconThreadPool.push(
		[this, iconResultID, item, internalIndex, basicItemInfo](int id)
		{
			UNREFERENCED_PARAMETER(id);

			return FindIconAsync(m_hTreeView, iconResultID, item, internalIndex,
				basicItemInfo.pidl.get());
		});

	m_iconResults.insert({ iconResultID, std::move(result) });
}

std::optional<ShellTreeView::IconResult> ShellTreeView::FindIconAsync(HWND treeView,
	int iconResultId, HTREEITEM item, int internalIndex, PCIDLIST_ABSOLUTE pidl)
{
	SHFILEINFO shfi;
	DWORD_PTR res = SHGetFileInfo(reinterpret_cast<LPCTSTR>(pidl), 0, &shfi, sizeof(SHFILEINFO),
		SHGFI_PIDL | SHGFI_ICON | SHGFI_OVERLAYINDEX);

	if (res == 0)
	{
		return std::nullopt;
	}

	DestroyIcon(shfi.hIcon);

	PostMessage(treeView, WM_APP_ICON_RESULT_READY, iconResultId, 0);

	IconResult result;
	result.item = item;
	result.internalIndex = internalIndex;
	result.iconIndex = shfi.iIcon;

	return result;
}

void ShellTreeView::ProcessIconResult(int iconResultId)
{
	auto iconResultsItr = m_iconResults.find(iconResultId);

	if (iconResultsItr == m_iconResults.end())
	{
		return;
	}

	auto cleanup = wil::scope_exit(
		[this, iconResultsItr]()
		{
			m_iconResults.erase(iconResultsItr);
		});

	auto result = iconResultsItr->second.get();

	if (!result)
	{
		return;
	}

	auto itemMapItr = m_itemInfoMap.find(result->internalIndex);

	// The item may have been removed (e.g. if the associated folder was deleted, or the parent was
	// collapsed).
	if (itemMapItr == m_itemInfoMap.end())
	{
		return;
	}

	const ItemInfo &itemInfo = itemMapItr->second;

	std::wstring filePath;
	HRESULT hr = GetDisplayName(itemInfo.pidl.get(), SHGDN_FORPARSING, filePath);

	if (SUCCEEDED(hr))
	{
		m_cachedIcons->addOrUpdateFileIcon(filePath, result->iconIndex);
	}

	TVITEM tvItem;
	tvItem.mask = TVIF_HANDLE | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE;
	tvItem.hItem = result->item;
	tvItem.iImage = result->iconIndex;
	tvItem.iSelectedImage = result->iconIndex;
	tvItem.stateMask = TVIS_OVERLAYMASK;
	tvItem.state = INDEXTOOVERLAYMASK(result->iconIndex >> 24);
	TreeView_SetItem(m_hTreeView, &tvItem);
}

void ShellTreeView::QueueSubfoldersTask(HTREEITEM item)
{
	BasicItemInfo basicItemInfo;
	basicItemInfo.pidl = GetItemPidl(item);

	int subfoldersResultID = m_subfoldersResultIDCounter++;

	auto result = m_subfoldersThreadPool.push(
		[this, subfoldersResultID, item, basicItemInfo](int id)
		{
			UNREFERENCED_PARAMETER(id);

			return CheckSubfoldersAsync(m_hTreeView, subfoldersResultID, item,
				basicItemInfo.pidl.get());
		});

	m_subfoldersResults.insert({ subfoldersResultID, std::move(result) });
}

std::optional<ShellTreeView::SubfoldersResult> ShellTreeView::CheckSubfoldersAsync(HWND treeView,
	int subfoldersResultId, HTREEITEM item, PCIDLIST_ABSOLUTE pidl)
{
	wil::com_ptr_nothrow<IShellFolder> pShellFolder;
	PCITEMID_CHILD pidlRelative;
	HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&pShellFolder), &pidlRelative);

	if (FAILED(hr))
	{
		return std::nullopt;
	}

	ULONG attributes = SFGAO_HASSUBFOLDER;
	hr = pShellFolder->GetAttributesOf(1, &pidlRelative, &attributes);

	if (FAILED(hr))
	{
		return std::nullopt;
	}

	PostMessage(treeView, WM_APP_SUBFOLDERS_RESULT_READY, subfoldersResultId, 0);

	SubfoldersResult result;
	result.item = item;
	result.hasSubfolder = WI_IsFlagSet(attributes, SFGAO_HASSUBFOLDER);

	return result;
}

void ShellTreeView::ProcessSubfoldersResult(int subfoldersResultId)
{
	auto itr = m_subfoldersResults.find(subfoldersResultId);

	if (itr == m_subfoldersResults.end())
	{
		return;
	}

	auto cleanup = wil::scope_exit(
		[this, itr]()
		{
			m_subfoldersResults.erase(itr);
		});

	auto result = itr->second.get();

	if (!result)
	{
		return;
	}

	if (result->hasSubfolder)
	{
		// By default it's assumed that an item has subfolders, so if it does
		// actually have subfolders, there's nothing else that needs to be done.
		return;
	}

	TVITEM tvItem;
	tvItem.mask = TVIF_HANDLE | TVIF_CHILDREN;
	tvItem.hItem = result->item;
	tvItem.cChildren = 0;
	TreeView_SetItem(m_hTreeView, &tvItem);
}

void ShellTreeView::OnItemExpanding(const NMTREEVIEW *nmtv)
{
	HTREEITEM parentItem = nmtv->itemNew.hItem;

	if (nmtv->action == TVE_EXPAND)
	{
		ExpandDirectory(parentItem);
	}
	else
	{
		auto hSelection = TreeView_GetSelection(m_hTreeView);

		if (hSelection != nullptr)
		{
			HTREEITEM hItem = hSelection;

			do
			{
				hItem = TreeView_GetParent(m_hTreeView, hItem);
			} while (hItem != parentItem && hItem != nullptr);

			// If the currently selected item is below the item being
			// collapsed, the selection should be adjusted to the parent item.
			if (hItem == parentItem)
			{
				TreeView_SelectItem(m_hTreeView, parentItem);
			}
		}

		RemoveChildrenFromInternalMap(parentItem);

		SendMessage(m_hTreeView, TVM_EXPAND, TVE_COLLAPSE | TVE_COLLAPSERESET,
			reinterpret_cast<LPARAM>(parentItem));

		ItemInfo &itemInfo = GetItemByHandle(parentItem);
		StopDirectoryMonitoringForItem(itemInfo);
	}
}

LRESULT ShellTreeView::OnKeyDown(const NMTVKEYDOWN *keyDown)
{
	switch (keyDown->wVKey)
	{
	case 'C':
		if (IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
		{
			CopySelectedItemToClipboard(true);
		}
		break;

	case 'X':
		if (IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
		{
			CopySelectedItemToClipboard(false);
		}
		break;

	case 'V':
		if (IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
		{
			Paste();
		}
		break;

	case VK_DELETE:
		if (IsKeyDown(VK_SHIFT))
		{
			DeleteSelectedItem(true);
		}
		else
		{
			DeleteSelectedItem(false);
		}
		break;
	}

	// If the ctrl key is down, this key sequence is likely a modifier. Stop any other pressed key
	// from been used in an incremental search.
	if (IsKeyDown(VK_CONTROL))
	{
		return 1;
	}

	return 0;
}

/* Sorts items in the following order:
 - Drives
 - Virtual Items
 - Real Items

Each set is ordered alphabetically. */
int CALLBACK CompareItemsStub(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
	ShellTreeView *shellTreeView = nullptr;

	shellTreeView = (ShellTreeView *) lParamSort;

	return shellTreeView->CompareItems(lParam1, lParam2);
}

int CALLBACK ShellTreeView::CompareItems(LPARAM lParam1, LPARAM lParam2)
{
	TCHAR szTemp[MAX_PATH];
	int iItemId1 = (int) lParam1;
	int iItemId2 = (int) lParam2;

	const ItemInfo &itemInfo1 = m_itemInfoMap.at(iItemId1);
	const ItemInfo &itemInfo2 = m_itemInfoMap.at(iItemId2);

	std::wstring displayName1;
	GetDisplayName(itemInfo1.pidl.get(), SHGDN_FORPARSING, displayName1);

	std::wstring displayName2;
	GetDisplayName(itemInfo2.pidl.get(), SHGDN_FORPARSING, displayName2);

	if (PathIsRoot(displayName1.c_str()) && !PathIsRoot(displayName2.c_str()))
	{
		return -1;
	}
	else if (!PathIsRoot(displayName1.c_str()) && PathIsRoot(displayName2.c_str()))
	{
		return 1;
	}
	else if (PathIsRoot(displayName1.c_str()) && PathIsRoot(displayName2.c_str()))
	{
		return lstrcmpi(displayName1.c_str(), displayName2.c_str());
	}
	else
	{
		if (!SHGetPathFromIDList(itemInfo1.pidl.get(), szTemp)
			&& SHGetPathFromIDList(itemInfo2.pidl.get(), szTemp))
		{
			return -1;
		}
		else if (SHGetPathFromIDList(itemInfo1.pidl.get(), szTemp)
			&& !SHGetPathFromIDList(itemInfo2.pidl.get(), szTemp))
		{
			return 1;
		}
		else
		{
			GetDisplayName(itemInfo1.pidl.get(), SHGDN_INFOLDER, displayName1);
			GetDisplayName(itemInfo2.pidl.get(), SHGDN_INFOLDER, displayName2);

			if (m_config->globalFolderSettings.useNaturalSortOrder)
			{
				return StrCmpLogicalW(displayName1.c_str(), displayName2.c_str());
			}
			else
			{
				return StrCmpIW(displayName1.c_str(), displayName2.c_str());
			}
		}
	}
}

HRESULT ShellTreeView::ExpandDirectory(HTREEITEM hParent)
{
	auto pidlDirectory = GetItemPidl(hParent);

	wil::com_ptr_nothrow<IShellFolder2> shellFolder2;
	HRESULT hr = BindToIdl(pidlDirectory.get(), IID_PPV_ARGS(&shellFolder2));

	if (FAILED(hr))
	{
		return hr;
	}

	SHCONTF enumFlags = SHCONTF_FOLDERS;

	if (m_bShowHidden)
	{
		enumFlags |= SHCONTF_INCLUDEHIDDEN | SHCONTF_INCLUDESUPERHIDDEN;
	}

	wil::com_ptr_nothrow<IEnumIDList> pEnumIDList;
	hr = shellFolder2->EnumObjects(nullptr, enumFlags, &pEnumIDList);

	if (FAILED(hr) || !pEnumIDList)
	{
		return hr;
	}

	SendMessage(m_hTreeView, WM_SETREDRAW, FALSE, 0);

	std::vector<unique_pidl_absolute> items;

	unique_pidl_child pidlItem;
	ULONG uFetched = 1;

	while (pEnumIDList->Next(1, wil::out_param(pidlItem), &uFetched) == S_OK && (uFetched == 1))
	{
		if (m_config->checkPinnedToNamespaceTreeProperty)
		{
			BOOL showItem = GetBooleanVariant(shellFolder2.get(), pidlItem.get(),
				&PKEY_IsPinnedToNameSpaceTree, TRUE);

			if (!showItem)
			{
				continue;
			}
		}

		if (m_config->globalFolderSettings.hideSystemFiles)
		{
			PCITEMID_CHILD child = pidlItem.get();
			SFGAOF attributes = SFGAO_SYSTEM;
			hr = shellFolder2->GetAttributesOf(1, &child, &attributes);

			if (FAILED(hr) || (WI_IsFlagSet(attributes, SFGAO_SYSTEM)))
			{
				continue;
			}
		}

		items.emplace_back(ILCombine(pidlDirectory.get(), pidlItem.get()));
	}

	for (const auto &item : items)
	{
		AddItem(hParent, item.get());
	}

	TVSORTCB tvscb;
	tvscb.hParent = hParent;
	tvscb.lpfnCompare = CompareItemsStub;
	tvscb.lParam = reinterpret_cast<LPARAM>(this);
	TreeView_SortChildrenCB(m_hTreeView, &tvscb, 0);

	SendMessage(m_hTreeView, WM_SETREDRAW, TRUE, 0);

	ItemInfo &itemInfo = GetItemByHandle(hParent);
	StartDirectoryMonitoringForItem(itemInfo);

	return hr;
}

void ShellTreeView::AddItem(HTREEITEM parent, PCIDLIST_ABSOLUTE pidl)
{
	std::wstring name;
	HRESULT hr = GetDisplayName(pidl, SHGDN_NORMAL, name);

	if (FAILED(hr))
	{
		return;
	}

	int itemId = GenerateUniqueItemId();
	m_itemInfoMap[itemId].pidl.reset(ILCloneFull(pidl));

	TVITEMEX tvItem = {};
	tvItem.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
	tvItem.pszText = name.data();
	tvItem.iImage = I_IMAGECALLBACK;
	tvItem.iSelectedImage = I_IMAGECALLBACK;
	tvItem.lParam = itemId;
	tvItem.cChildren = I_CHILDRENCALLBACK;

	TVINSERTSTRUCT tvInsertData = {};
	tvInsertData.hInsertAfter = TVI_LAST;
	tvInsertData.hParent = parent;
	tvInsertData.itemex = tvItem;

	[[maybe_unused]] auto item = TreeView_InsertItem(m_hTreeView, &tvInsertData);
	assert(item);
}

int ShellTreeView::GenerateUniqueItemId()
{
	return m_itemIDCounter++;
}

unique_pidl_absolute ShellTreeView::GetSelectedItemPidl() const
{
	auto selectedItem = TreeView_GetSelection(m_hTreeView);
	return GetItemPidl(selectedItem);
}

unique_pidl_absolute ShellTreeView::GetItemPidl(HTREEITEM hTreeItem) const
{
	const ItemInfo &itemInfo = GetItemByHandle(hTreeItem);
	unique_pidl_absolute pidl(ILCloneFull(itemInfo.pidl.get()));
	return pidl;
}

const ShellTreeView::ItemInfo &ShellTreeView::GetItemByHandle(HTREEITEM item) const
{
	int internalIndex = GetItemInternalIndex(item);
	return m_itemInfoMap.at(internalIndex);
}

ShellTreeView::ItemInfo &ShellTreeView::GetItemByHandle(HTREEITEM item)
{
	int internalIndex = GetItemInternalIndex(item);
	return m_itemInfoMap.at(internalIndex);
}

int ShellTreeView::GetItemInternalIndex(HTREEITEM item) const
{
	TVITEMEX tvItemEx;
	tvItemEx.mask = TVIF_HANDLE | TVIF_PARAM;
	tvItemEx.hItem = item;
	[[maybe_unused]] bool res = TreeView_GetItem(m_hTreeView, &tvItemEx);
	assert(res);

	return static_cast<int>(tvItemEx.lParam);
}

HTREEITEM ShellTreeView::LocateItem(PCIDLIST_ABSOLUTE pidlDirectory)
{
	return LocateItemInternal(pidlDirectory, FALSE);
}

HTREEITEM ShellTreeView::LocateExistingItem(PCIDLIST_ABSOLUTE pidlDirectory)
{
	return LocateItemInternal(pidlDirectory, TRUE);
}

HTREEITEM ShellTreeView::LocateItemInternal(PCIDLIST_ABSOLUTE pidlDirectory,
	BOOL bOnlyLocateExistingItem)
{
	HTREEITEM hRoot;
	HTREEITEM hItem;
	TVITEMEX item;
	BOOL bFound = FALSE;

	/* Get the root of the tree (root of namespace). */
	hRoot = TreeView_GetRoot(m_hTreeView);
	hItem = hRoot;

	item.mask = TVIF_PARAM | TVIF_HANDLE;
	item.hItem = hItem;
	TreeView_GetItem(m_hTreeView, &item);

	/* Keep searching until the specified item
	is found or it is found the item does not
	exist in the treeview.
	Look through each item, once an ancestor is
	found, look through it's children, expanding
	the parent node if necessary. */
	while (!bFound && hItem != nullptr)
	{
		if (ArePidlsEquivalent(m_itemInfoMap.at(static_cast<int>(item.lParam)).pidl.get(),
				pidlDirectory))
		{
			bFound = TRUE;

			break;
		}

		if (ILIsParent(m_itemInfoMap.at(static_cast<int>(item.lParam)).pidl.get(), pidlDirectory,
				FALSE))
		{
			if ((TreeView_GetChild(m_hTreeView, hItem)) == nullptr)
			{
				if (bOnlyLocateExistingItem)
				{
					return nullptr;
				}
				else
				{
					SendMessage(m_hTreeView, TVM_EXPAND, TVE_EXPAND, (LPARAM) hItem);
				}
			}

			hItem = TreeView_GetChild(m_hTreeView, hItem);
		}
		else
		{
			hItem = TreeView_GetNextSibling(m_hTreeView, hItem);
		}

		item.mask = TVIF_PARAM | TVIF_HANDLE;
		item.hItem = hItem;
		TreeView_GetItem(m_hTreeView, &item);
	}

	return hItem;
}

void ShellTreeView::RemoveChildrenFromInternalMap(HTREEITEM hParent)
{
	auto hItem = TreeView_GetChild(m_hTreeView, hParent);

	while (hItem != nullptr)
	{
		TVITEMEX tvItemEx;
		tvItemEx.mask = TVIF_PARAM | TVIF_HANDLE | TVIF_CHILDREN;
		tvItemEx.hItem = hItem;
		TreeView_GetItem(m_hTreeView, &tvItemEx);

		if (tvItemEx.cChildren != 0)
		{
			RemoveChildrenFromInternalMap(hItem);
		}

		m_itemInfoMap.erase(static_cast<int>(tvItemEx.lParam));

		hItem = TreeView_GetNextSibling(m_hTreeView, hItem);
	}
}

void ShellTreeView::OnMiddleButtonDown(const POINT *pt)
{
	TVHITTESTINFO hitTestInfo;
	hitTestInfo.pt = *pt;

	TreeView_HitTest(m_hTreeView, &hitTestInfo);

	if (hitTestInfo.flags != LVHT_NOWHERE)
	{
		m_middleButtonItem = hitTestInfo.hItem;
	}
	else
	{
		m_middleButtonItem = nullptr;
	}
}

void ShellTreeView::OnMiddleButtonUp(const POINT *pt, UINT keysDown)
{
	TVHITTESTINFO hitTestInfo;
	hitTestInfo.pt = *pt;

	TreeView_HitTest(m_hTreeView, &hitTestInfo);

	if (hitTestInfo.flags == LVHT_NOWHERE)
	{
		return;
	}

	// Only open an item if it was the one on which the middle mouse button was initially clicked
	// on.
	if (hitTestInfo.hItem != m_middleButtonItem)
	{
		return;
	}

	bool switchToNewTab = m_config->openTabsInForeground;

	if (WI_IsFlagSet(keysDown, MK_SHIFT))
	{
		switchToNewTab = !switchToNewTab;
	}

	auto pidl = GetItemPidl(hitTestInfo.hItem);
	m_tabContainer->CreateNewTab(pidl.get(), TabSettings(_selected = switchToNewTab));
}

void ShellTreeView::SetShowHidden(BOOL bShowHidden)
{
	m_bShowHidden = bShowHidden;
}

void ShellTreeView::RefreshAllIcons()
{
	auto hRoot = TreeView_GetRoot(m_hTreeView);

	TVITEMEX tvItemEx;
	tvItemEx.mask = TVIF_HANDLE | TVIF_PARAM;
	tvItemEx.hItem = hRoot;
	TreeView_GetItem(m_hTreeView, &tvItemEx);

	const ItemInfo &itemInfo = m_itemInfoMap.at(static_cast<int>(tvItemEx.lParam));

	SHFILEINFO shfi;
	SHGetFileInfo(reinterpret_cast<LPCTSTR>(itemInfo.pidl.get()), 0, &shfi, sizeof(shfi),
		SHGFI_PIDL | SHGFI_SYSICONINDEX);

	tvItemEx.mask = TVIF_HANDLE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
	tvItemEx.hItem = hRoot;
	tvItemEx.iImage = shfi.iIcon;
	tvItemEx.iSelectedImage = shfi.iIcon;
	TreeView_SetItem(m_hTreeView, &tvItemEx);

	RefreshAllIconsInternal(TreeView_GetChild(m_hTreeView, hRoot));
}

void ShellTreeView::RefreshAllIconsInternal(HTREEITEM hFirstSibling)
{
	HTREEITEM hNextSibling;
	HTREEITEM hChild;
	TVITEM tvItem;
	SHFILEINFO shfi;

	hNextSibling = TreeView_GetNextSibling(m_hTreeView, hFirstSibling);

	tvItem.mask = TVIF_HANDLE | TVIF_PARAM;
	tvItem.hItem = hFirstSibling;
	TreeView_GetItem(m_hTreeView, &tvItem);

	const ItemInfo &itemInfo = m_itemInfoMap[static_cast<int>(tvItem.lParam)];
	SHGetFileInfo(reinterpret_cast<LPCTSTR>(itemInfo.pidl.get()), 0, &shfi, sizeof(shfi),
		SHGFI_PIDL | SHGFI_SYSICONINDEX);

	tvItem.mask = TVIF_HANDLE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
	tvItem.hItem = hFirstSibling;
	tvItem.iImage = shfi.iIcon;
	tvItem.iSelectedImage = shfi.iIcon;
	TreeView_SetItem(m_hTreeView, &tvItem);

	hChild = TreeView_GetChild(m_hTreeView, hFirstSibling);

	if (hChild != nullptr)
		RefreshAllIconsInternal(hChild);

	while (hNextSibling != nullptr)
	{
		tvItem.mask = TVIF_HANDLE | TVIF_PARAM;
		tvItem.hItem = hNextSibling;
		TreeView_GetItem(m_hTreeView, &tvItem);

		const ItemInfo &itemInfoNext = m_itemInfoMap[static_cast<int>(tvItem.lParam)];
		SHGetFileInfo(reinterpret_cast<LPCTSTR>(itemInfoNext.pidl.get()), 0, &shfi, sizeof(shfi),
			SHGFI_PIDL | SHGFI_SYSICONINDEX);

		tvItem.mask = TVIF_HANDLE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
		tvItem.hItem = hNextSibling;
		tvItem.iImage = shfi.iIcon;
		tvItem.iSelectedImage = shfi.iIcon;
		TreeView_SetItem(m_hTreeView, &tvItem);

		hChild = TreeView_GetChild(m_hTreeView, hNextSibling);

		if (hChild != nullptr)
			RefreshAllIconsInternal(hChild);

		hNextSibling = TreeView_GetNextSibling(m_hTreeView, hNextSibling);
	}
}

HRESULT ShellTreeView::OnBeginDrag(int iItemId)
{
	wil::com_ptr_nothrow<IDataObject> dataObject;
	std::vector<PCIDLIST_ABSOLUTE> items = { m_itemInfoMap.at(iItemId).pidl.get() };
	RETURN_IF_FAILED(CreateDataObjectForShellTransfer(items, &dataObject));

	DWORD effect;
	return SHDoDragDrop(m_hTreeView, dataObject.get(), nullptr,
		DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

void ShellTreeView::StartRenamingSelectedItem()
{
	auto selectedItem = TreeView_GetSelection(m_hTreeView);
	TreeView_EditLabel(m_hTreeView, selectedItem);
}

void ShellTreeView::ShowPropertiesOfSelectedItem() const
{
	auto pidlDirectory = GetSelectedItemPidl();
	ShowMultipleFileProperties(pidlDirectory.get(), {}, m_hTreeView);
}

void ShellTreeView::DeleteSelectedItem(bool permanent)
{
	HTREEITEM item = TreeView_GetSelection(m_hTreeView);
	HTREEITEM parentItem = TreeView_GetParent(m_hTreeView, item);

	// Select the parent item to release the lock and allow deletion.
	TreeView_Select(m_hTreeView, parentItem, TVGN_CARET);

	auto pidl = GetItemPidl(item);

	DWORD mask = 0;

	if (permanent)
	{
		mask = CMIC_MASK_SHIFT_DOWN;
	}

	ExecuteActionFromContextMenu(pidl.get(), {}, m_hTreeView, _T("delete"), mask, nullptr);
}

bool ShellTreeView::OnEndLabelEdit(const NMTVDISPINFO *dispInfo)
{
	// If label editing was canceled or no text was entered, simply notify the control to revert to
	// the previous text.
	if (!dispInfo->item.pszText || lstrlen(dispInfo->item.pszText) == 0)
	{
		return false;
	}

	const auto &itemInfo = GetItemByHandle(dispInfo->item.hItem);

	std::wstring oldFileName;
	HRESULT hr = GetDisplayName(itemInfo.pidl.get(), SHGDN_FORPARSING, oldFileName);

	if (FAILED(hr))
	{
		return false;
	}

	TCHAR newFileName[MAX_PATH];
	StringCchCopy(newFileName, SIZEOF_ARRAY(newFileName), oldFileName.c_str());
	PathRemoveFileSpec(newFileName);
	bool res = PathAppend(newFileName, dispInfo->item.pszText);

	if (!res)
	{
		return false;
	}

	FileActionHandler::RenamedItem_t renamedItem;
	renamedItem.strOldFilename = oldFileName;
	renamedItem.strNewFilename = newFileName;

	TrimStringRight(renamedItem.strNewFilename, _T(" "));

	std::list<FileActionHandler::RenamedItem_t> renamedItemList;
	renamedItemList.push_back(renamedItem);
	m_fileActionHandler->RenameFiles(renamedItemList);

	return true;
}

void ShellTreeView::CopySelectedItemToClipboard(bool copy)
{
	HTREEITEM item = TreeView_GetSelection(m_hTreeView);
	auto &itemInfo = GetItemByHandle(item);

	std::vector<PCIDLIST_ABSOLUTE> items = { itemInfo.pidl.get() };
	wil::com_ptr_nothrow<IDataObject> clipboardDataObject;
	HRESULT hr;

	if (copy)
	{
		hr = CopyFiles(items, &clipboardDataObject);

		if (SUCCEEDED(hr))
		{
			UpdateCurrentClipboardObject(clipboardDataObject);
		}
	}
	else
	{
		hr = CutFiles(items, &clipboardDataObject);

		if (SUCCEEDED(hr))
		{
			UpdateCurrentClipboardObject(clipboardDataObject);

			m_cutItem = item;
			UpdateItemState(item, TVIS_CUT, TVIS_CUT);
		}
	}
}

void ShellTreeView::Paste()
{
	wil::com_ptr_nothrow<IDataObject> clipboardObject;
	HRESULT hr = OleGetClipboard(&clipboardObject);

	if (FAILED(hr))
	{
		return;
	}

	auto &selectedItem = GetItemByHandle(TreeView_GetSelection(m_hTreeView));

	if (CanShellPasteDataObject(selectedItem.pidl.get(), clipboardObject.get(),
			DROPEFFECT_COPY | DROPEFFECT_MOVE))
	{
		ExecuteActionFromContextMenu(selectedItem.pidl.get(), {}, m_hTreeView, L"paste", 0,
			nullptr);
	}
	else
	{
		std::wstring destinationPath;
		hr = GetDisplayName(selectedItem.pidl.get(), SHGDN_FORPARSING, destinationPath);

		if (FAILED(hr))
		{
			return;
		}

		DropHandler *dropHandler = DropHandler::CreateNew();
		dropHandler->CopyClipboardData(clipboardObject.get(), m_hTreeView, destinationPath.c_str(),
			nullptr);
		dropHandler->Release();
	}
}

void ShellTreeView::PasteShortcut()
{
	auto &selectedItem = GetItemByHandle(TreeView_GetSelection(m_hTreeView));
	ExecuteActionFromContextMenu(selectedItem.pidl.get(), {}, m_hTreeView, L"pastelink", 0,
		nullptr);
}

void ShellTreeView::UpdateCurrentClipboardObject(
	wil::com_ptr_nothrow<IDataObject> clipboardDataObject)
{
	// When copying an item, the WM_CLIPBOARDUPDATE message will be processed after the copy
	// operation has been fully completed. Therefore, any previously cut item will need to have its
	// state restored first. Relying on the WM_CLIPBOARDUPDATE handler wouldn't work, as by the time
	// it runs, m_cutItem would refer to the newly cut item.
	if (m_cutItem)
	{
		UpdateItemState(m_cutItem, TVIS_CUT, 0);
	}

	m_clipboardDataObject = clipboardDataObject;
}

void ShellTreeView::OnClipboardUpdate()
{
	if (m_clipboardDataObject && OleIsCurrentClipboard(m_clipboardDataObject.get()) == S_FALSE)
	{
		if (m_cutItem)
		{
			UpdateItemState(m_cutItem, TVIS_CUT, 0);

			m_cutItem = nullptr;
		}

		m_clipboardDataObject.reset();
	}
}

void ShellTreeView::UpdateItemState(HTREEITEM item, UINT stateMask, UINT state)
{
	TVITEM tvItem;
	tvItem.mask = TVIF_HANDLE | TVIF_STATE;
	tvItem.hItem = item;
	tvItem.stateMask = stateMask;
	tvItem.state = state;
	[[maybe_unused]] bool res = TreeView_SetItem(m_hTreeView, &tvItem);
	assert(res);
}
