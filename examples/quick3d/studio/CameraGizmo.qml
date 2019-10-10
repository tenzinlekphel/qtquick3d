/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

import QtQuick 2.0
import QtQuick3D 1.0

/*
    The gizmo showing the rotation of the camera differs from other
    gizmos by the fact that it doesn't overlay the camera in the
    scene, but rather stays in a corner of the view. Still, the
    perspective of the gizmo should reflect the perspective as if
    it was located on the same spot as the camera. For that reason, we
    draw it in a separate View3D, which means that the user can position
    it wherever he wants on the screen without affecting how it looks.
 */
View3D {
    id: root
    property Camera targetCamera
    implicitWidth: 50
    implicitHeight: 50
    camera: localCamera

    function updateGizmo()
    {
        sceneGizmo.position = localCamera.mapFromViewport(Qt.vector3d(0.5, 0.5, 180))

        var xLabelScenePos = sceneGizmo.arrowX.mapPositionToScene(Qt.vector3d(0, 2, -12));
        var xLabelViewPos = root.mapFrom3DScene(xLabelScenePos)
        xLabel.x = xLabelViewPos.x - xLabel.width
        xLabel.y = xLabelViewPos.y - xLabel.height

        var yLabelScenePos = sceneGizmo.arrowY.mapPositionToScene(Qt.vector3d(4, 0, -9.5));
        var yLabelViewPos = root.mapFrom3DScene(yLabelScenePos)
        yLabel.x = yLabelViewPos.x - yLabel.width
        yLabel.y = yLabelViewPos.y - yLabel.height

        var zLabelScenePos = sceneGizmo.arrowZ.mapPositionToScene(Qt.vector3d(0, 2, -12));
        var zLabelViewPos = root.mapFrom3DScene(zLabelScenePos)
        zLabel.x = zLabelViewPos.x - zLabel.width
        zLabel.y = zLabelViewPos.y - zLabel.height
    }

    scene: Node {
        Camera {
            id: localCamera
            position: targetCamera.scenePosition
            rotation: targetCamera.sceneRotation
        }

        MoveGizmo {
            id: sceneGizmo
            Connections {
                target: localCamera
                onSceneTransformChanged: updateGizmo()
            }
        }

        Connections {
            target: window
            onFirstFrameReady: updateGizmo()
        }
    }

    Text {
        id: xLabel
        text: "x"
        color: "red"
    }

    Text {
        id: yLabel
        text: "y"
        color: "blue"
    }

    Text {
        id: zLabel
        text: "z"
        color: "green"
    }

}