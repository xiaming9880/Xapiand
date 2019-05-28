/** @file honey_document.h
 * @brief A document read from a HoneyDatabase.
 */
/* Copyright (C) 2008,2009,2010,2011,2017 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef XAPIAN_INCLUDED_HONEY_DOCUMENT_H
#define XAPIAN_INCLUDED_HONEY_DOCUMENT_H

#include "xapian/backends/honey/honey_docdata.h"
#include "xapian/backends/honey/honey_values.h"
#include "xapian/backends/databaseinternal.h"
#include "xapian/backends/documentinternal.h"

/// A document read from a HoneyDatabase.
class HoneyDocument : public Xapian::Document::Internal {
    /// Don't allow assignment.
    void operator=(const HoneyDocument &);

    /// Don't allow copying.
    HoneyDocument(const HoneyDocument &);

    /// Used for lazy access to document values.
    const HoneyValueManager *value_manager;

    /// Used for lazy access to document data.
    const HoneyDocDataTable *docdata_table;

    /// HoneyDatabase::open_document() needs to call our private constructor.
    friend class HoneyDatabase;

    /// Private constructor - only called by HoneyDatabase::open_document().
    HoneyDocument(const Xapian::Database::Internal* db,
		  Xapian::docid did_,
		  const HoneyValueManager *value_manager_,
		  const HoneyDocDataTable *docdata_table_)
	: Xapian::Document::Internal(db, did_),
	  value_manager(value_manager_), docdata_table(docdata_table_) { }

  protected:
    /** Implementation of virtual methods @{ */
    string fetch_value(Xapian::valueno slot) const;
    void fetch_all_values(btree::map<Xapian::valueno, string>& values_) const;
    string fetch_data() const;
    /** @} */
};

#endif // XAPIAN_INCLUDED_HONEY_DOCUMENT_H
