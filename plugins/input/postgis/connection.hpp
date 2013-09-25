/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2011 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#ifndef POSTGIS_CONNECTION_HPP
#define POSTGIS_CONNECTION_HPP

// mapnik
#include <mapnik/debug.hpp>
#include <mapnik/datasource.hpp>
#include <mapnik/timer.hpp>

// std
#include <memory>
#include <sstream>
#include <iostream>

extern "C" {
#include "libpq-fe.h"
}

#include "resultset.hpp"

class Connection
{
public:
    Connection(std::string const& connection_str,boost::optional<std::string> const& password)
        : cursorId(0),
          closed_(false)
    {
        std::string connect_with_pass = connection_str;
        if (password && !password->empty())
        {
            connect_with_pass += " password=" + *password;
        }
        conn_ = PQconnectdb(connect_with_pass.c_str());
        if (PQstatus(conn_) != CONNECTION_OK)
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nConnection string: '";
            err_msg += connection_str;
            err_msg += "'\n";
            throw mapnik::datasource_exception(err_msg);
        }
    }

    ~Connection()
    {
        if (! closed_)
        {
            PQfinish(conn_);
            MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: postgresql connection closed - " << conn_;
            closed_ = true;
        }
    }

    bool execute(std::string const& sql) const
    {
#ifdef MAPNIK_STATS
        mapnik::progress_timer __stats__(std::clog, std::string("postgis_connection::execute ") + sql);
#endif

        PGresult *result = PQexec(conn_, sql.c_str());
        bool ok = (result && (PQresultStatus(result) == PGRES_COMMAND_OK));
        PQclear(result);
        return ok;
    }

    std::shared_ptr<ResultSet> executeQuery(std::string const& sql, int type = 0) const
    {
#ifdef MAPNIK_STATS
        mapnik::progress_timer __stats__(std::clog, std::string("postgis_connection::execute_query ") + sql);
#endif

        PGresult* result = 0;
        if (type == 1)
        {
            result = PQexecParams(conn_,sql.c_str(), 0, 0, 0, 0, 0, 1);
        }
        else
        {
            result = PQexec(conn_, sql.c_str());
        }

        if (! result || (PQresultStatus(result) != PGRES_TUPLES_OK))
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin executeQuery Full sql was: '";
            err_msg += sql;
            err_msg += "'\n";
            if (result)
            {
                PQclear(result);
            }
            throw mapnik::datasource_exception(err_msg);
        }

        return std::make_shared<ResultSet>(result);
    }

    std::string status() const
    {
        std::string status;
        if (conn_)
        {
            status = PQerrorMessage(conn_);
        }
        else
        {
            status = "Uninitialized connection";
        }
        return status;
    }

    bool executeAsyncQuery(std::string const& sql, int type = 0)
    {
        int result = 0;
        if (type == 1)
        {
            result = PQsendQueryParams(conn_,sql.c_str(), 0, 0, 0, 0, 0, 1);
        }
        else
        {
            result = PQsendQuery(conn_, sql.c_str());
        }
        if (result != 1)
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin executeAsyncQuery Full sql was: '";
            err_msg += sql;
            err_msg += "'\n";
            clearAsyncResult(PQgetResult(conn_));
            close();
            throw mapnik::datasource_exception(err_msg);
        }
        return result;
    }


    std::shared_ptr<ResultSet> getNextAsyncResult()
    {
        PGresult *result = PQgetResult(conn_);
        if( result && (PQresultStatus(result) != PGRES_TUPLES_OK))
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin getNextAsyncResult";
            clearAsyncResult(result);
            // We need to guarde against losing the connection
            // (i.e db restart) so here we invalidate the full connection
            close();
            throw mapnik::datasource_exception(err_msg);
        }
       return std::make_shared<ResultSet>(result);
    }

    std::shared_ptr<ResultSet> getAsyncResult()
    {
        PGresult *result = PQgetResult(conn_);
        if ( !result || (PQresultStatus(result) != PGRES_TUPLES_OK))
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin getAsyncResult";
            clearAsyncResult(result);
            // We need to be guarded against losing the connection
            // (i.e db restart), we invalidate the full connection
            close();
            throw mapnik::datasource_exception(err_msg);
        }
        return std::make_shared<ResultSet>(result);
    }

    std::string client_encoding() const
    {
        return PQparameterStatus(conn_, "client_encoding");
    }

    bool isOK() const
    {
        return (!closed_) && (PQstatus(conn_) != CONNECTION_BAD);
    }

    void close()
    {
        if (! closed_)
        {
            PQfinish(conn_);
            MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: datasource closed, also closing connection - " << conn_;
            closed_ = true;
        }
    }

    std::string new_cursor_name()
    {
        std::ostringstream s;
        s << "mapnik_" << (cursorId++);
        return s.str();
    }

private:
    PGconn *conn_;
    int cursorId;
    bool closed_;

    void clearAsyncResult(PGresult *result) const
    {
        // Clear all pending results
        while(result)
        {
           PQclear(result);
           result = PQgetResult(conn_);
        }
    }
};

#endif //CONNECTION_HPP
