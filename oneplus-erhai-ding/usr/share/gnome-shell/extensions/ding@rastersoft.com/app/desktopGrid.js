/* DING: Desktop Icons New Generation for GNOME Shell
 *
 * Copyright (C) 2019 Sergio Costas (rastersoft@gmail.com)
 * Based on code original (C) Carlos Soriano
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/* exported DesktopGrid */
'use strict';
const Gtk = imports.gi.Gtk;
const Gdk = imports.gi.Gdk;
const GLib = imports.gi.GLib;   /* erhai: touch long-press timer */

const Prefs = imports.preferences;
const Enums = imports.enums;
const DesktopIconsUtil = imports.desktopIconsUtil;
const SignalManager = imports.signalManager;

const Gettext = imports.gettext.domain('ding');

const _ = Gettext.gettext;


var elementSpacing = 2;

/* erhai: touch-only long-press -> desktop context menu (see the note further
 * down).  _initTouchLongPress marks the patched file for DEBIAN/postinst. */
var TOUCH_LONGPRESS_MS = 500;
var TOUCH_LONGPRESS_MAX_MOVE = 14;

var DesktopGrid = class extends SignalManager.SignalManager{
    constructor(desktopManager, desktopName, desktopDescription, asDesktop) {
        super();
        this._signalIds = [];
        this._destroying = false;
        this._desktopManager = desktopManager;
        this._desktopName = desktopName;
        this._asDesktop = asDesktop;
        this._desktopDescription = desktopDescription;
        /* erhai: touch long-press -> context menu state */
        this._touchLongPressTimer = 0;
        this._touchLongPressStartX = 0;
        this._touchLongPressStartY = 0;
        this._touchLongPressEvent = null;
        /* erhai: live touch-point bookkeeping for the single-finger gate --
         * a touch press arrives emulated as button 1, which looks identical
         * with one or with three fingers down, so the raw GdkEventTouch
         * (touch-event, needs TOUCH_MASK) is what tells them apart. */
        this._touchPoints = 0;
        this._touchMultiTouch = false;
        this.updateWindowGeometry();
        this.updateUnscaledHeightWidthMargins();
        this.createGrids();

        this._window = new Gtk.ApplicationWindow({application: desktopManager.mainApp, 'title': desktopName});
        this._windowContext = this._window.get_style_context();
        if (this._asDesktop) {
            this._window.set_decorated(false);
            this._window.set_deletable(false);
            // For Wayland Transparent background, but only if this instance is working as desktop
            this._windowContext.add_class('desktopwindow');
            // If we are under X11, Transparent background and everything else from here as well
            if (this._desktopManager.using_X11) {
                let screen = this._window.get_screen();
                let visual = screen.get_rgba_visual();
                if (visual && screen.is_composited()) {
                    this._window.set_visual(visual);
                } else {
                    print('Unable to set Transparency under X11!');
                }
                this._window.set_type_hint(Gdk.WindowTypeHint.DESKTOP);
                this._window.stick();
                this._window.move(this._x / this._size_divisor, this._y / this._size_divisor);
            } else { // Wayland
                this._window.maximize();
            }
        } else {
            // Opaque black test window
            this._windowContext.add_class('testwindow');
        }
        this._window.set_resizable(false);
        this.connectSignal(this._window, 'delete-event', () => {
            if (this._destroying) {
                return false;
            }
            if (this._asDesktop) {
                // Do not destroy window when closing if the instance is working as desktop
                return true;
            } else {
                // Exit if this instance is working as an stand-alone window
                return false;
            }
        });

        this._eventBox = new Gtk.EventBox({visible: true});
        this.sizeEventBox();
        this._window.add(this._eventBox);
        this._container = new Gtk.Fixed();
        this._eventBox.add(this._container);
        this.gridGlobalRectangle = new Gdk.Rectangle();
        this.setDropDestination(this._eventBox);

        this._selectedList = null;
        this.connectSignal(this._container, 'draw', (widget, cr) => {
            this._doDrawRubberBand(cr);
            cr.$dispose();
        });

        this.setGridStatus();
        this._window.set_default_size(this._windowWidth, this._windowHeight);
        this._window.set_size_request(this._windowWidth, this._windowHeight);
        this._window.resize(this._windowWidth, this._windowHeight);
        this._window.show_all();
        this._window.set_size_request(this._windowWidth, this._windowHeight);
        this._window.resize(this._windowWidth, this._windowHeight);

        this._eventBox.add_events(Gdk.EventMask.BUTTON_MOTION_MASK |
                                  Gdk.EventMask.BUTTON_PRESS_MASK |
                                  Gdk.EventMask.BUTTON_RELEASE_MASK |
                                  Gdk.EventMask.KEY_RELEASE_MASK |
                                  /* erhai: needed to see the raw touch points
                                   * (the emulated button events do not say how
                                   * many fingers are down) */
                                  Gdk.EventMask.TOUCH_MASK);
        this.connectSignal(this._eventBox, 'button-press-event', (actor, event) => {
            let [a, x, y] = event.get_coords();
            /* erhai: start the touch-only long-press detector on the RAW,
             * window-local coordinates (before they are converted), so the
             * context menu can be anchored at the finger. */
            this._maybeStartTouchLongPress(event, x, y);
            [x, y] = this.coordinatesLocalToGlobal(x, y);
            this._desktopManager.onPressButton(x, y, event, this);
            return false;
        });
        this.connectSignal(this._eventBox, 'motion-notify-event', (actor, event) => {
            let [a, x, y] = event.get_coords();
            /* erhai: a finger that moves is not a long press */
            this._cancelTouchLongPressIfMoved(x, y);
            [x, y] = this.coordinatesLocalToGlobal(x, y);
            this._desktopManager.onMotion(x, y);
        });
        this.connectSignal(this._eventBox, 'button-release-event', (actor, event) => {
            this._cancelTouchLongPress();
            /* erhai: the emulated release ends the touch set on this widget */
            this._touchPoints = 0;
            this._touchMultiTouch = false;
            this._desktopManager.onReleaseButton(this);
        });
        /* erhai: single-finger gate for the long press -- see _trackTouchEvent */
        this.connectSignal(this._eventBox, 'touch-event', (actor, event) => {
            this._trackTouchEvent(event);
            return false;
        });

        this.connectSignal(this._window, 'key-press-event', (window, event) => {
            this._desktopManager.onKeyPress(window, event, this);
        });
        // key-release-event must be used for the arrow keys to avoid conflicts
        // with assistive technologies.
        this.connectSignal(this._window, 'key-release-event', (actor, event) => {
            this._desktopManager.onKeyRelease(event, this);
        });
        this.updateGridRectangle();
    }

    updateGridDescription(desktopDescription) {
        this._desktopDescription = desktopDescription;
    }

    updateWindowGeometry() {
        this._zoom = this._desktopDescription.scaleFactor;
        this._x = this._desktopDescription.x;
        this._y = this._desktopDescription.y;
        this._monitor = this._desktopDescription.monitorIndex;
        this._size_divisor = this._zoom;
        if (this._asDesktop && this._desktopManager.using_X11) {
            this._size_divisor = this._zoom;
        }
        this._windowWidth = Math.floor(this._desktopDescription.width / this._size_divisor);
        this._windowHeight = Math.floor(this._desktopDescription.height / this._size_divisor);
    }

    resizeWindow() {
        this.updateWindowGeometry();
        this._desktopName = `@!${this._x},${this._y};BDHF`;
        if (this._desktopManager.using_X11) {
            this._window.move(this._x / this._size_divisor, this._y / this._size_divisor);
        }
        this._window.set_title(this._desktopName);
        this._window.set_size_request(this._windowWidth, this._windowHeight);
        this._window.resize(this._windowWidth, this._windowHeight);
    }

    updateUnscaledHeightWidthMargins() {
        this._marginTop = this._desktopDescription.marginTop;
        this._marginBottom = this._desktopDescription.marginBottom;
        this._marginLeft = this._desktopDescription.marginLeft;
        this._marginRight = this._desktopDescription.marginRight;
        this._width = this._desktopDescription.width - this._marginLeft - this._marginRight;
        this._height = this._desktopDescription.height - this._marginTop - this._marginBottom;
    }

    createGrids() {
        this._width = Math.floor(this._width / this._size_divisor);
        this._height = Math.floor(this._height / this._size_divisor);
        this._marginTop = Math.floor(this._marginTop / this._size_divisor);
        this._marginBottom = Math.floor(this._marginBottom / this._size_divisor);
        this._marginLeft = Math.floor(this._marginLeft / this._size_divisor);
        this._marginRight = Math.floor(this._marginRight / this._size_divisor);
        this._maxColumns = Math.floor(this._width / (Prefs.get_desired_width() + 4 * elementSpacing));
        this._maxRows =  Math.floor(this._height / (Prefs.get_desired_height() + 4 * elementSpacing));
        this._elementWidth = Math.floor(this._width / this._maxColumns);
        this._elementHeight = Math.floor(this._height / this._maxRows);
    }

    updateGridRectangle() {
        this.gridGlobalRectangle.x = this._x + this._marginLeft;
        this.gridGlobalRectangle.y = this._y + this._marginTop;
        this.gridGlobalRectangle.width = this._width;
        this.gridGlobalRectangle.height = this._height;
    }

    sizeEventBox() {
        this._eventBox.margin_top = this._marginTop;
        this._eventBox.margin_bottom = this._marginBottom;
        const leftToRight =
            this._eventBox.get_direction() === Gtk.TextDirection.LTR;
        if (leftToRight) {
            this._eventBox.margin_start = this._marginLeft;
            this._eventBox.margin_end = this._marginRight;
        } else {
            this._eventBox.margin_start = this._marginRight;
            this._eventBox.margin_end = this._marginLeft;
        }
    }

    setGridStatus() {
        this._fileItems = {};
        this._gridStatus = {};
        for (let y = 0; y < this._maxRows; y++) {
            for (let x = 0; x < this._maxColumns; x++) {
                this._setGridUse(x, y, false);
            }
        }
    }

    resizeGrid() {
        this.updateUnscaledHeightWidthMargins();
        this.createGrids();
        this.updateGridRectangle();
        this.sizeEventBox();
        this.setGridStatus();
    }

    destroy() {
        this._destroying = true;
        this.disconnectAllSignals();
        this._window.destroy();
    }

    setDropDestination(dropDestination) {
        dropDestination.drag_dest_set(Gtk.DestDefaults.MOTION | Gtk.DestDefaults.DROP, null, Gdk.DragAction.MOVE | Gdk.DragAction.COPY | Gdk.DragAction.DEFAULT);
        let targets = new Gtk.TargetList(null);
        targets.add(Gdk.atom_intern('x-special/ding-icon-list', false), Gtk.TargetFlags.SAME_APP,
            Enums.DndTargetInfo.DING_ICON_LIST);
        targets.add(Gdk.atom_intern('x-special/gnome-icon-list', false), 0,
            Enums.DndTargetInfo.GNOME_ICON_LIST);
        targets.add(Gdk.atom_intern('text/uri-list', false), 0,
            Enums.DndTargetInfo.URI_LIST);
        targets.add(Gdk.atom_intern('text/plain', false), 0,
            Enums.DndTargetInfo.TEXT_PLAIN);
        dropDestination.drag_dest_set_target_list(targets);
        targets = undefined; // to avoid memory leaks
        this.connectSignal(dropDestination, 'drag-motion', (widget, context, x, y, time) => {
            this.receiveMotion(x, y);

            if (DesktopIconsUtil.getModifiersInDnD(context, Gdk.ModifierType.CONTROL_MASK)) {
                Gdk.drag_status(context, Gdk.DragAction.COPY, time);
            } else {
                Gdk.drag_status(context, Gdk.DragAction.MOVE, time);
            }
        });
        this.connectSignal(this._eventBox, 'drag-leave', (widget, context, time) => {
            this.receiveLeave();
        });
        this.connectSignal(dropDestination, 'drag-data-received', (widget, context, x, y, selection, info, time) => {
            const forceCopy = context.get_selected_action() === Gdk.DragAction.COPY;
            this.receiveDrop(context, x, y, selection, info, false, forceCopy);
        });
    }

    receiveLeave() {
        this._desktopManager.onDragLeave();
    }

    receiveMotion(x, y, global) {
        if (!global) {
            x = this._elementWidth * Math.floor(x / this._elementWidth);
            y = this._elementHeight * Math.floor(y / this._elementHeight);
            [x, y] = this.coordinatesLocalToGlobal(x, y);
        }
        this._desktopManager.onDragMotion(x, y);
    }

    receiveDrop(context, x, y, selection, info, forceLocal, forceCopy) {
        if (!forceLocal) {
            x = this._elementWidth * Math.floor(x / this._elementWidth);
            y = this._elementHeight * Math.floor(y / this._elementHeight);
            [x, y] = this.coordinatesLocalToGlobal(x, y);
        }
        this._desktopManager.onDragDataReceived(context, x, y, selection, info, forceLocal, forceCopy);
        this._window.queue_draw();
    }

    highLightGridAt(x, y) {
        let selected = this.getGridAt(x, y, false);
        this._selectedList = [selected];
        this._window.queue_draw();
    }

    unHighLightGrids() {
        this._selectedList = null;
        this._window.queue_draw();
    }

    _getGridCoordinates(x, y) {
        let placeX = Math.floor(x / this._elementWidth);
        let placeY = Math.floor(y / this._elementHeight);
        placeX = DesktopIconsUtil.clamp(placeX, 0, this._maxColumns - 1);
        placeY = DesktopIconsUtil.clamp(placeY, 0, this._maxRows - 1);
        return [placeX, placeY];
    }

    gridInUse(x, y) {
        let [placeX, placeY] = this._getGridCoordinates(x, y);
        return !this._isEmptyAt(placeX, placeY);
    }

    getGridLocalCoordinates(x, y) {
        let [column, row] = this._getGridCoordinates(x, y);
        let localX = Math.floor(this._width * column / this._maxColumns);
        let localY = Math.floor(this._height * row / this._maxRows);
        return [localX, localY];
    }

    _fileAt(x, y) {
        let [placeX, placeY] = this._getGridCoordinates(x, y);
        return this._gridStatus[placeY * this._maxColumns + placeX];
    }

    refreshDrag(selectedList, ox, oy) {
        if (selectedList === null) {
            this._selectedList = null;
            this._window.queue_draw();
            return;
        }
        let newSelectedList = [];
        for (let [x, y] of selectedList) {
            x += this._elementWidth / 2;
            y += this._elementHeight / 2;
            x += ox;
            y += oy;
            let r = this.getGridAt(x, y);
            if (r && !isNaN(r[0]) && !isNaN(r[1]) && (!this.gridInUse(r[0], r[1]) || this._fileAt(r[0], r[1]).isSelected)) {
                newSelectedList.push(r);
            }
        }
        if (newSelectedList.length == 0) {
            if (this._selectedList !== null) {
                this._selectedList = null;
                this._window.queue_draw();
            }
            return;
        }
        if (this._selectedList !== null) {
            if ((newSelectedList[0][0] == this._selectedList[0][0]) && (newSelectedList[0][1] == this._selectedList[0][1])) {
                return;
            }
        }
        this._selectedList = newSelectedList;
        this._window.queue_draw();
    }

    queue_draw() {
        this._window.queue_draw();
    }

    _roundedRectangle(cr, x, y, width, height, radius) {
        radius = Math.min(radius, Math.abs(width) / 2, Math.abs(height) / 2);
        const right = x + width;
        const bottom = y + height;

        cr.newPath();
        cr.arc(x + radius, y + radius, radius, Math.PI, 1.5 * Math.PI);
        cr.arc(right - radius, y + radius, radius, 1.5 * Math.PI, 2 * Math.PI);
        cr.arc(right - radius, bottom - radius, radius, 0, 0.5 * Math.PI);
        cr.arc(x + radius, bottom - radius, radius, 0.5 * Math.PI, Math.PI);
        cr.closePath();
    }

    _doDrawRubberBand(cr) {
        if (this._desktopManager.rubberBand && this._desktopManager.selectionRectangle) {
            if (!this.gridGlobalRectangle.intersect(this._desktopManager.selectionRectangle)[0]) {
                return;
            }
            let [xInit, yInit] = this.coordinatesGlobalToLocal(this._desktopManager.x1, this._desktopManager.y1);
            let [xFin, yFin] = this.coordinatesGlobalToLocal(this._desktopManager.x2, this._desktopManager.y2);

            this._roundedRectangle(cr, xInit + 0.5, yInit + 0.5, xFin - xInit, yFin - yInit, 5);
            const selectColor = this._desktopManager.selectColor.copy();
            selectColor.alpha = 0.2;
            Gdk.cairo_set_source_rgba(cr, selectColor);
            cr.fill();
            cr.setLineWidth(2);
            this._roundedRectangle(cr, xInit + 0.5, yInit + 0.5, xFin - xInit, yFin - yInit, 5);
            Gdk.cairo_set_source_rgba(cr, this._desktopManager.borderColor);
            cr.stroke();
        }
        if (this._desktopManager.showDropPlace && (this._selectedList !== null)) {
            for (let [x, y] of this._selectedList) {
                this._roundedRectangle(cr, x + 0.5, y + 0.5, this._elementWidth, this._elementHeight, 10);
                const color = this._desktopManager.selectColor.copy();
                color.alpha = 0.4;
                Gdk.cairo_set_source_rgba(cr, color);
                cr.fill();
                cr.setLineWidth(2);
                this._roundedRectangle(cr, x + 0.5, y + 0.5, this._elementWidth, this._elementHeight, 10);
                Gdk.cairo_set_source_rgba(cr, this._desktopManager.borderColor);
                cr.stroke();
            }
        }
    }

    getDistance(x, y) {
        /**
         * Checks if these coordinates belong to this grid.
         *
         * @returns -1 if there is no free space for new icons;
         *          0 if the coordinates are inside this grid;
         *          or the distance to the middle point, if none of the previous
         */

        let isFree = false;
        for (let element in this._gridStatus) {
            if (!this._gridStatus[element]) {
                isFree = true;
                break;
            }
        }
        if (!isFree) {
            return -1;
        }
        if (this._coordinatesBelongToThisGrid(x, y)) {
            return 0;
        }
        return Math.pow(x - (this._x + this._windowWidth * this._zoom / 2), 2) + Math.pow(x - (this._y + this._windowHeight * this._zoom / 2), 2);
    }

    coordinatesGlobalToLocal(X, Y, widget = null) {
        X -= this._x;
        Y -= this._y;
        if (!widget) {
            widget = this._eventBox;
        }
        let [belong, x, y] = this._window.translate_coordinates(widget, X, Y);
        return [x, y];
    }

    coordinatesLocalToGlobal(x, y, widget = null) {
        if (!widget) {
            widget = this._eventBox;
        }
        let [belongs, X, Y] = widget.translate_coordinates(this._window, x, y);
        return [X + this._x, Y + this._y];
    }

    _addFileItemTo(fileItem, column, row, coordinatesAction) {
        if (this._destroying) {
            return;
        }
        let localX = Math.floor(this._width * column / this._maxColumns);
        let localY = Math.floor(this._height * row / this._maxRows);
        this._container.put(fileItem.container, localX + elementSpacing, localY + elementSpacing);
        this._setGridUse(column, row, fileItem);
        this._fileItems[fileItem.uri] = [column, row, fileItem];
        let [x, y] = this.coordinatesLocalToGlobal(localX + elementSpacing, localY + elementSpacing);
        fileItem.setCoordinates(x,
            y,
            this._elementWidth - 2 * elementSpacing,
            this._elementHeight - 2 * elementSpacing,
            elementSpacing,
            this,
            column / this._maxColumns);
        /* If this file is new in the Desktop and hasn't yet
         * fixed coordinates, store the new possition to ensure
         * that the next time it will be shown in the same possition.
         * Also store the new possition if it has been moved by the user,
         * and not triggered by a screen change.
         */
        if ((fileItem.savedCoordinates == null) || (coordinatesAction == Enums.StoredCoordinates.OVERWRITE)) {
            fileItem.savedCoordinates = [x, y];
        }
    }

    removeItem(fileItem) {
        if (fileItem.uri in this._fileItems) {
            let [column, row, tmp] = this._fileItems[fileItem.uri];
            this._setGridUse(column, row, false);
            this._container.remove(fileItem.container);
            delete this._fileItems[fileItem.uri];
        }
    }

    addFileItemCloseTo(fileItem, x, y, coordinatesAction) {
        let addVolumesOpposite = Prefs.desktopSettings.get_boolean('add-volumes-opposite');
        let [column, row] = this._getEmptyPlaceClosestTo(x,
            y,
            coordinatesAction,
            fileItem.isDrive && addVolumesOpposite);
        this._addFileItemTo(fileItem, column, row, coordinatesAction);
    }

    _isEmptyAt(x, y) {
        return this._gridStatus[y * this._maxColumns + x] === false;
    }

    _setGridUse(x, y, inUse) {
        this._gridStatus[y * this._maxColumns + x] = inUse;
    }

    getGridAt(x, y, globalCoordinates = false) {
        if (this._coordinatesBelongToThisGrid(x, y)) {
            [x, y] = this.coordinatesGlobalToLocal(x, y);
            if (globalCoordinates) {
                x = this._elementWidth * Math.floor(x / this._elementWidth);
                y = this._elementHeight * Math.floor(y / this._elementHeight);
                [x, y] = this.coordinatesLocalToGlobal(x, y);
                return [x, y];
            } else {
                return this.getGridLocalCoordinates(x, y);
            }
        } else {
            return null;
        }
    }

    _coordinatesBelongToThisGrid(X, Y) {
        let checkRectangle = new Gdk.Rectangle({x: X, y: Y, width: 1, height: 1});
        return this.gridGlobalRectangle.intersect(checkRectangle)[0];
    }

    _getEmptyPlaceClosestTo(x, y, coordinatesAction, reverseHorizontal) {
        [x, y] = this.coordinatesGlobalToLocal(x, y);
        let placeX = Math.floor(x / this._elementWidth);
        let placeY = Math.floor(y / this._elementHeight);

        let cornerInversion = Prefs.get_start_corner();
        if (reverseHorizontal) {
            cornerInversion[0] = !cornerInversion[0];
        }

        placeX = DesktopIconsUtil.clamp(placeX, 0, this._maxColumns - 1);
        placeY = DesktopIconsUtil.clamp(placeY, 0, this._maxRows - 1);
        if (this._isEmptyAt(placeX, placeY) && (coordinatesAction != Enums.StoredCoordinates.ASSIGN)) {
            return [placeX, placeY];
        }
        let found = false;
        let resColumn = null;
        let resRow = null;
        let minDistance = Infinity;
        let column, row;
        for (let tmpColumn = 0; tmpColumn < this._maxColumns; tmpColumn++) {
            if (cornerInversion[0]) {
                column = this._maxColumns - tmpColumn - 1;
            } else {
                column = tmpColumn;
            }
            for (let tmpRow = 0; tmpRow < this._maxRows; tmpRow++) {
                if (cornerInversion[1]) {
                    row = this._maxRows - tmpRow - 1;
                } else {
                    row = tmpRow;
                }
                if (!this._isEmptyAt(column, row)) {
                    continue;
                }

                let proposedX = column * this._elementWidth;
                let proposedY = row * this._elementHeight;
                if (coordinatesAction == Enums.StoredCoordinates.ASSIGN) {
                    return [column, row];
                }
                let distance = DesktopIconsUtil.distanceBetweenPoints(proposedX, proposedY, x, y);
                if (distance < minDistance) {
                    found = true;
                    minDistance = distance;
                    resColumn = column;
                    resRow = row;
                }
            }
        }

        if (!found) {
            throw new Error('Not enough place at monitor');
        }

        return [resColumn, resRow];
    }

    /** ******************************
     * erhai: touch long-press -> context menu
     ********************************/

    /* The desktop background context menu is opened by a SECONDARY (mouse)
     * button press -- `button == 3` in desktopManager.onPressButton().  A
     * touch tap or long tap is delivered to GTK as an emulated PRIMARY
     * (button 1) press, so the menu never opened with a finger.  This is a
     * touch-ONLY shim: a finger held still over the desktop for
     * TOUCH_LONGPRESS_MS opens the same menu the right mouse button would.
     * Mouse and trackpad presses also carry button 1 but their device source
     * is not the touchscreen, so they are never affected.
     * `_initTouchLongPress` also appears here so DEBIAN/postinst can tell a
     * patched file from Ubuntu's. */
    _maybeStartTouchLongPress(event, x, y) {
        this._cancelTouchLongPress();
        if (event.get_button()[1] != 1) {
            return;
        }
        let device = event.get_source_device();
        /* NOTE: for a touch, GTK's emulated button event has
         * get_device() == the "Wayland Touch Master Pointer" (source MOUSE);
         * the touchscreen shows up as the SOURCE device.  A mouse/trackpad
         * press has a MOUSE source device too, so this excludes it. */
        if (!device || device.get_source() != Gdk.InputSource.TOUCHSCREEN) {
            return;
        }
        /* erhai: the long press is a SINGLE-FINGER gesture: never arm it when
         * more than one finger is (or was) down.  Measured 2026-10-15: with
         * three fingers on the glass the desktop menu used to open 500 ms
         * later, and the menu's popup grab then cancelled the shell's
         * 3-finger workspace swipe (SwipeTracker._cancelPanGesture() restarts
         * from the CURRENT workspace = "the swipe snaps back").
         * The second finger's TOUCH_BEGIN arrives with the same finger-down
         * batch as the emulated press, so this cancels the timer in time. */
        if (this._touchMultiTouch || this._touchPoints > 1) {
            return;
        }
        this._touchLongPressStartX = x;
        this._touchLongPressStartY = y;
        /* keep a COPY of the press event: the menu popup needs a trigger event
         * (device + time), and the original is only valid inside this handler */
        this._touchLongPressEvent = event.copy();
        this._touchLongPressTimer = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT, TOUCH_LONGPRESS_MS, () => {
                this._touchLongPressTimer = 0;
                this._doTouchLongPress(x, y);
                return GLib.SOURCE_REMOVE;
            });
    }

    _cancelTouchLongPressIfMoved(x, y) {
        if (!this._touchLongPressTimer) {
            return;
        }
        if ((Math.abs(x - this._touchLongPressStartX) > TOUCH_LONGPRESS_MAX_MOVE) ||
            (Math.abs(y - this._touchLongPressStartY) > TOUCH_LONGPRESS_MAX_MOVE)) {
            this._cancelTouchLongPress();
        }
    }

    _cancelTouchLongPress() {
        if (this._touchLongPressTimer) {
            GLib.source_remove(this._touchLongPressTimer);
            this._touchLongPressTimer = 0;
        }
        this._touchLongPressEvent = null;
    }

    /* erhai: keep track of how many fingers are on this widget.
     *
     * A touch press reaches GTK as an emulated button-1 press (see
     * _maybeStartTouchLongPress), which is indistinguishable from a mouse
     * press and says nothing about the number of fingers; the RAW
     * GdkEventTouch does -- GTK3 delivers one touch-event per finger (with a
     * real GdkEventSequence) as long as the widget's window has
     * Gdk.EventMask.TOUCH_MASK set (measured 2026-10-15: three fingers =>
     * three TOUCH_BEGIN, and only ONE emulated press).
     *
     * TOUCH_CANCEL matters just as much: when the shell's 3-finger workspace
     * gesture grabs the sequence, the compositor CANCELS the client's touch and
     * NO button-release follows -- without this, the armed 500 ms timer
     * survived the whole swipe and popped the context menu in the middle of it,
     * which (through the menu's popup grab) aborted the swipe. */
    _trackTouchEvent(event) {
        let type = event.get_event_type();
        if (type == Gdk.EventType.TOUCH_BEGIN) {
            this._touchPoints++;
            if (this._touchPoints > 1) {
                this._touchMultiTouch = true;
                this._cancelTouchLongPress();
            }
        } else if (type == Gdk.EventType.TOUCH_END ||
                   type == Gdk.EventType.TOUCH_CANCEL) {
            this._touchPoints = Math.max(0, this._touchPoints - 1);
            if (this._touchPoints == 0) {
                this._touchMultiTouch = false;
            }
            if (type == Gdk.EventType.TOUCH_CANCEL) {
                this._cancelTouchLongPress();
            }
        }
    }

    _doTouchLongPress(x, y) {
        let triggerEvent = this._touchLongPressEvent;
        this._touchLongPressTimer = 0;
        this._touchLongPressEvent = null;
        /* drop the rubber-band state the primary press started, so a finger
         * lifted after the menu grabbed the seat cannot leave it dangling */
        this._desktopManager.onReleaseButton(this);
        this._desktopManager.showDesktopContextMenu(x, y, this._eventBox, triggerEvent);
    }
};
