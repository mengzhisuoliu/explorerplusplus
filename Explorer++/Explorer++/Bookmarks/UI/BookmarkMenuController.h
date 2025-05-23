// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#pragma once

class AcceleratorManager;
class BookmarkItem;
class BookmarkTree;
class BrowserWindow;
class ClipboardStore;
class ResourceLoader;

class BookmarkMenuController
{
public:
	BookmarkMenuController(BookmarkTree *bookmarkTree, BrowserWindow *browser,
		const AcceleratorManager *acceleratorManager, const ResourceLoader *resourceLoader,
		HWND parentWindow, ClipboardStore *clipboardStore);

	void OnMenuItemSelected(const BookmarkItem *bookmarkItem, bool isCtrlKeyDown,
		bool isShiftKeyDown);
	void OnMenuItemMiddleClicked(const BookmarkItem *bookmarkItem, bool isCtrlKeyDown,
		bool isShiftKeyDown);
	void OnMenuItemRightClicked(BookmarkItem *bookmarkItem, const POINT &pt);

private:
	BookmarkTree *const m_bookmarkTree;
	BrowserWindow *const m_browser;
	const AcceleratorManager *const m_acceleratorManager;
	const ResourceLoader *const m_resourceLoader;
	const HWND m_parentWindow;
	ClipboardStore *const m_clipboardStore;
};
