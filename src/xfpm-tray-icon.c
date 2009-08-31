/*
 * * Copyright (C) 2008-2009 Ali <aliov@xfce.org>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <libxfce4util/libxfce4util.h>
#include <libxfcegui4/libxfcegui4.h>

#include "libxfpm/xfpm-common.h"
#include "libxfpm/xfpm-string.h"
#include "libxfpm/xfpm-notify.h"
#include "libxfpm/xfpm-icons.h"

#include "xfpm-tray-icon.h"
#include "xfpm-shutdown.h"
#include "xfpm-dbus-monitor.h"
#include "xfpm-inhibit.h"
#include "xfpm-screen-saver.h"
#include "xfpm-xfconf.h"
#include "xfpm-config.h"

static void xfpm_tray_icon_finalize   (GObject *object);

#define XFPM_TRAY_ICON_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE((o), XFPM_TYPE_TRAY_ICON, XfpmTrayIconPrivate))

struct XfpmTrayIconPrivate
{
    XfpmShutdown    *shutdown;
    XfpmXfconf      *conf;
    XfpmNotify      *notify;
    XfpmInhibit     *inhibit;
    XfpmScreenSaver *srv;
    XfpmDBusMonitor *monitor;
    
    GtkStatusIcon   *icon;
    GQuark           icon_quark;
    gboolean         info_menu;
    gboolean         inhibited;
    gboolean	     data_available;
    
    gulong           sig;
    gulong	     sig_1;
};

enum
{
    SHOW_INFORMATION,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE(XfpmTrayIcon, xfpm_tray_icon, G_TYPE_OBJECT)

static gboolean
xfpm_tray_icon_size_changed_cb (GtkStatusIcon *icon, gint size, XfpmTrayIcon *tray)
{
    GdkPixbuf *pix;
    
    g_return_val_if_fail (size > 0, FALSE);

    if ( tray->priv->icon_quark == 0 )
	return FALSE;
    
    pix = xfce_themed_icon_load (g_quark_to_string (tray->priv->icon_quark), size);
    
    if ( pix )
    {
	gtk_status_icon_set_from_pixbuf (GTK_STATUS_ICON(tray->priv->icon), pix);
	g_object_unref (pix);
	return TRUE;
    }
    return FALSE;
}

static void
xfpm_tray_info (GtkWidget *w, XfpmTrayIcon *tray)
{
    g_signal_emit (G_OBJECT (tray), signals[SHOW_INFORMATION], 0);
}

static gboolean
xfpm_tray_icon_do_suspend (XfpmTrayIcon *tray)
{
    GError *error = NULL;

    xfpm_suspend (tray->priv->shutdown, &error);

    if (error)
    {
	g_warning ("%s", error->message);
	xfpm_notify_show_notification (tray->priv->notify, 
				      _("Xfce power manager"), 
				       error->message, 
				       xfpm_tray_icon_get_icon_name (tray),
				       10000,
				       FALSE,
				       XFPM_NOTIFY_CRITICAL,
				       tray->priv->icon);
	g_error_free (error);
    }
    return FALSE;
}

static gboolean
xfpm_tray_icon_do_hibernate (XfpmTrayIcon *tray)
{
    GError *error = NULL;

    xfpm_hibernate (tray->priv->shutdown, &error);
    
    if (error)
    {
	g_warning ("%s", error->message);
	xfpm_notify_show_notification (tray->priv->notify, 
				      _("Xfce power manager"), 
				       error->message, 
				       xfpm_tray_icon_get_icon_name (tray),
				       10000,
				       FALSE,
				       XFPM_NOTIFY_CRITICAL,
				       tray->priv->icon);
	g_error_free (error);
    }
    return FALSE;
}

static void
xfpm_tray_icon_hibernate_cb (GtkWidget *w, XfpmTrayIcon *tray)
{
    const gchar *message;
    gboolean lock_screen;
    gboolean ret;

    g_object_get (G_OBJECT (tray->priv->conf),
		  LOCK_SCREEN_ON_SLEEP, &lock_screen,
		  NULL);
    
    message = tray->priv->inhibited ? 
	     _("An application is currently disabling the automatic sleep,"
	      " doing this action now may damage the working state of this application,"
	      " are you sure you want to hibernate the system?") :
	      _("Are you sure you want to proceed?") ;
    
    ret = 
	xfce_confirm (message,
		      GTK_STOCK_YES,
		      _("Hibernate"));
    
    if ( ret ) 
    {
	xfpm_shutdown_add_callback (tray->priv->shutdown, 
				    (GSourceFunc) xfpm_tray_icon_do_hibernate, 
				    lock_screen,
				    tray);
    }
}

static void
xfpm_tray_icon_suspend_cb (GtkWidget *w, XfpmTrayIcon *tray)
{
    const gchar *message;
    gboolean lock_screen;
    gboolean ret;
    
    g_object_get (G_OBJECT (tray->priv->conf),
		  LOCK_SCREEN_ON_SLEEP, &lock_screen,
		  NULL);
    
    message = tray->priv->inhibited ? 
	     _("An application is currently disabling the automatic sleep,"
	      " doing this action now may damage the working state of this application,"
	      " are you sure you want to suspend the system?") :
	      _("Are you sure you want to proceed?") ;
    
    ret = 
	xfce_confirm  (message,
		       GTK_STOCK_YES,
		       _("Suspend"));
    
    if ( ret ) 
    {
	xfpm_shutdown_add_callback (tray->priv->shutdown, 
				    (GSourceFunc) xfpm_tray_icon_do_suspend, 
				    lock_screen,
				    tray);
    }
}

static void
xfpm_tray_icon_exit_activated_cb (GtkWidget *w, gpointer data)
{
    gboolean ret;
    
    ret = xfce_confirm (_("Quit the Xfce power manager"),
	                GTK_STOCK_YES,
			_("Quit"));
    if ( ret )
    {
	xfpm_quit ();
    }
}

static void
xfpm_tray_icon_menu_selection_done (GtkMenuShell *menu, gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (menu));
}

static void
xfpm_tray_icon_inhibit_active_cb (GtkWidget *mi, XfpmTrayIcon *tray)
{
    gboolean active;
    active = xfpm_screen_saver_get_inhibit (tray->priv->srv);
    
    active == FALSE ? xfpm_screen_saver_inhibit   (tray->priv->srv) :
		      xfpm_screen_saver_uninhibit (tray->priv->srv);
}

static void
xfpm_tray_icon_popup_menu_cb (GtkStatusIcon *icon, guint button, 
			      guint activate_time, XfpmTrayIcon *tray)
{
    GtkWidget *menu, *mi, *img;
    gboolean can_suspend = FALSE;
    gboolean can_hibernate = FALSE ;
    gboolean caller = FALSE;

    gboolean saver_inhibited;

    menu = gtk_menu_new();

    g_object_get (G_OBJECT (tray->priv->shutdown),
		  "caller-privilege", &caller,
		  "can-suspend", &can_suspend,
		  "can-hibernate", &can_hibernate,
		  NULL);
    
    // Hibernate menu option
    mi = gtk_image_menu_item_new_with_label (_("Hibernate"));
    img = gtk_image_new_from_icon_name (XFPM_HIBERNATE_ICON, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), img);
    gtk_widget_set_sensitive (mi,FALSE);
    
    if ( caller && can_hibernate && tray->priv->data_available )
    {
	gtk_widget_set_sensitive (mi, TRUE);
	g_signal_connect (G_OBJECT (mi), "activate",
			  G_CALLBACK (xfpm_tray_icon_hibernate_cb), tray);
    }
    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
    // Suspend menu option
    mi = gtk_image_menu_item_new_with_label(_("Suspend"));
    img = gtk_image_new_from_icon_name (XFPM_SUSPEND_ICON, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),img);
    
    gtk_widget_set_sensitive(mi,FALSE);
    if ( caller && can_suspend && tray->priv->data_available )
    {
	gtk_widget_set_sensitive (mi,TRUE);
	g_signal_connect (mi, "activate",
			  G_CALLBACK (xfpm_tray_icon_suspend_cb), tray);
    }
    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);

    saver_inhibited = xfpm_screen_saver_get_inhibit (tray->priv->srv);
    mi = gtk_check_menu_item_new_with_label (_("Monitor power control"));
    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (mi), !saver_inhibited);
    gtk_widget_set_tooltip_text (mi, _("Disable or enable monitor power control, "\
                                       "for example you could disable the screen power "\
				       "control to avoid screen blanking when watching a movie."));
    
    g_signal_connect (G_OBJECT (mi), "activate",
		      G_CALLBACK (xfpm_tray_icon_inhibit_active_cb), tray);
    gtk_widget_set_sensitive (mi, TRUE);
    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
    if ( tray->priv->info_menu )
    {
	mi = gtk_separator_menu_item_new();
	gtk_widget_show(mi);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
	// Battery informations
    
	mi = gtk_image_menu_item_new_from_stock (GTK_STOCK_INFO, NULL);
	
	gtk_widget_set_sensitive(mi,FALSE);
	gtk_widget_set_sensitive (mi,TRUE);
	
	g_signal_connect(mi,"activate",
			 G_CALLBACK(xfpm_tray_info), tray);
			 
	gtk_widget_show(mi);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
    
	// Separator
	mi = gtk_separator_menu_item_new();
	gtk_widget_show(mi);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    }
	
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_HELP,NULL);
    gtk_widget_set_sensitive(mi,TRUE);
    gtk_widget_show(mi);
    g_signal_connect(mi,"activate",G_CALLBACK(xfpm_help),NULL);
	
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
    
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT,NULL);
    gtk_widget_set_sensitive(mi,TRUE);
    gtk_widget_show(mi);
    g_signal_connect(mi,"activate",G_CALLBACK(xfpm_about), _("Xfce Power Manager"));
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);
    
    mi = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES,NULL);
    gtk_widget_set_sensitive(mi,TRUE);
    gtk_widget_show(mi);
    g_signal_connect(mi,"activate",G_CALLBACK(xfpm_preferences),NULL);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),mi);

    mi = gtk_separator_menu_item_new ();
    gtk_widget_show (mi);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);

    mi = gtk_image_menu_item_new_from_stock (GTK_STOCK_QUIT, NULL);
    gtk_widget_set_sensitive (mi, TRUE);
    gtk_widget_show (mi);
    g_signal_connect (mi, "activate", G_CALLBACK (xfpm_tray_icon_exit_activated_cb), NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);

    g_signal_connect (menu, "selection-done",
		      G_CALLBACK (xfpm_tray_icon_menu_selection_done), NULL);

    // Popup the menu
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
		   gtk_status_icon_position_menu, 
		   icon, button, activate_time);
}

static void
xfpm_tray_icon_inhibit_changed_cb (XfpmInhibit *inhibit, gboolean inhibited, XfpmTrayIcon *tray)
{
    tray->priv->inhibited = inhibited;
}

static void
xfpm_tray_icon_hal_connection_changed_cb (XfpmDBusMonitor *monitor, gboolean connected, XfpmTrayIcon *tray)
{
    if ( connected == FALSE )
	xfpm_tray_icon_set_tooltip (tray, _("No data available"));
	
    /*
     * We set this variable after since xfpm_tray_icon_set_tooltip will return 
     * if data_available is set to FALSE
     */
    tray->priv->data_available = connected;
}

static void
xfpm_tray_icon_activated_cb (XfpmTrayIcon *tray)
{
    xfpm_preferences ();
}

static void
xfpm_tray_icon_class_init(XfpmTrayIconClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    signals[SHOW_INFORMATION] =
            g_signal_new("show-information",
                         XFPM_TYPE_TRAY_ICON,
                         G_SIGNAL_RUN_LAST,
                         G_STRUCT_OFFSET(XfpmTrayIconClass, show_info),
                         NULL, NULL,
                         g_cclosure_marshal_VOID__VOID,
                         G_TYPE_NONE, 0, G_TYPE_NONE);

    object_class->finalize = xfpm_tray_icon_finalize;

    g_type_class_add_private(klass,sizeof(XfpmTrayIconPrivate));
}

static void
xfpm_tray_icon_init(XfpmTrayIcon *tray)
{
    tray->priv = XFPM_TRAY_ICON_GET_PRIVATE(tray);
    
    tray->priv->icon  = gtk_status_icon_new();
    tray->priv->shutdown = xfpm_shutdown_new ();
    tray->priv->conf  = xfpm_xfconf_new ();
    tray->priv->notify = xfpm_notify_new ();
    tray->priv->inhibited = FALSE;
    tray->priv->inhibit = xfpm_inhibit_new ();
    tray->priv->srv     = xfpm_screen_saver_new ();
    tray->priv->monitor = xfpm_dbus_monitor_new ();
    
    tray->priv->info_menu = TRUE;
    tray->priv->icon_quark = 0;
    
    g_signal_connect (tray->priv->icon, "size-changed",
		      G_CALLBACK (xfpm_tray_icon_size_changed_cb), tray);
		      
    g_signal_connect (tray->priv->icon, "popup-menu",
		      G_CALLBACK (xfpm_tray_icon_popup_menu_cb), tray);
		      
    g_signal_connect_swapped (tray->priv->icon, "activate",
			      G_CALLBACK (xfpm_tray_icon_activated_cb), tray);
			      
    tray->priv->sig = g_signal_connect (tray->priv->inhibit, "has-inhibit-changed",
					G_CALLBACK (xfpm_tray_icon_inhibit_changed_cb), tray);
					
    tray->priv->sig_1 = g_signal_connect (tray->priv->monitor, "hal-connection-changed",
					 G_CALLBACK (xfpm_tray_icon_hal_connection_changed_cb), tray);
					 
    tray->priv->data_available = xfpm_dbus_monitor_hal_connected (tray->priv->monitor);
}

static void
xfpm_tray_icon_finalize(GObject *object)
{
    XfpmTrayIcon *tray;

    tray = XFPM_TRAY_ICON(object);
    
    if ( g_signal_handler_is_connected (tray->priv->inhibit, tray->priv->sig ) )
	g_signal_handler_disconnect (G_OBJECT (tray->priv->inhibit), tray->priv->sig);
    
    if ( g_signal_handler_is_connected (tray->priv->monitor, tray->priv->sig_1) )
	g_signal_handler_disconnect (G_OBJECT (tray->priv->monitor), tray->priv->sig_1);
	
    g_object_unref (tray->priv->monitor);
    
    g_object_unref (tray->priv->icon);
	
    g_object_unref (tray->priv->shutdown);
    
    g_object_unref (tray->priv->conf);
    
    g_object_unref (tray->priv->notify);
    
    g_object_unref (tray->priv->inhibit);
    
    g_object_unref (tray->priv->srv);

    G_OBJECT_CLASS(xfpm_tray_icon_parent_class)->finalize(object);
}

XfpmTrayIcon *
xfpm_tray_icon_new(void)
{
    XfpmTrayIcon *tray = NULL;
    tray = g_object_new (XFPM_TYPE_TRAY_ICON, NULL);
    return tray;
}

void xfpm_tray_icon_set_show_info_menu (XfpmTrayIcon *icon, gboolean value)
{
    g_return_if_fail (XFPM_IS_TRAY_ICON (icon));
    icon->priv->info_menu = value;
}

void xfpm_tray_icon_set_icon (XfpmTrayIcon *icon, const gchar *icon_name)
{
    g_return_if_fail(XFPM_IS_TRAY_ICON(icon));
    
    icon->priv->icon_quark = g_quark_from_string(icon_name);
    
    xfpm_tray_icon_size_changed_cb (icon->priv->icon,
				    gtk_status_icon_get_size(icon->priv->icon),
				    icon);
}

void xfpm_tray_icon_set_tooltip (XfpmTrayIcon *icon, const gchar *tooltip)
{
    g_return_if_fail (XFPM_IS_TRAY_ICON (icon));
    /*
     * Hal is disconnected we have the tooltips set to "no data available"
     */
    g_return_if_fail (icon->priv->data_available);

#if GTK_CHECK_VERSION (2, 16, 0)
    gtk_status_icon_set_tooltip_text (GTK_STATUS_ICON(icon->priv->icon), tooltip);
#else
    gtk_status_icon_set_tooltip (GTK_STATUS_ICON(icon->priv->icon), tooltip);
#endif
}

void xfpm_tray_icon_set_visible (XfpmTrayIcon *icon, gboolean visible)
{
    g_return_if_fail(XFPM_IS_TRAY_ICON(icon));
    
    gtk_status_icon_set_visible(GTK_STATUS_ICON(icon->priv->icon), visible);
}

gboolean xfpm_tray_icon_get_visible (XfpmTrayIcon *icon)
{
    g_return_val_if_fail (XFPM_IS_TRAY_ICON(icon), FALSE);
    
    return gtk_status_icon_get_visible (GTK_STATUS_ICON(icon->priv->icon));
}

GtkStatusIcon *xfpm_tray_icon_get_tray_icon (XfpmTrayIcon *icon)
{
    g_return_val_if_fail(XFPM_IS_TRAY_ICON(icon), NULL);
    
    return icon->priv->icon;
}

const gchar *xfpm_tray_icon_get_icon_name   (XfpmTrayIcon *icon)
{
    g_return_val_if_fail(XFPM_IS_TRAY_ICON(icon), NULL);
    
    if ( icon->priv->icon_quark == 0 ) return NULL;
    
    return  g_quark_to_string (icon->priv->icon_quark);
}
