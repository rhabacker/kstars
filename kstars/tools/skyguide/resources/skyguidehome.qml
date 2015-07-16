import QtQuick 2.2
import QtQuick.Layouts 1.1

ColumnLayout {
    id: home
    spacing: 10
    Layout.alignment: Qt.AlignHCenter

    Text {
        anchors.topMargin: 5
        anchors.fill: parent
        horizontalAlignment: Text.AlignHCenter
        color: "#1703ca"
        font.pixelSize: 24
        style: Text.Raised
        text: "Sky Guide"
    }

    ObjRectangle {
        Component {
            id: guidesDelegate
            Item {
                property var modelData: model.modelData
                width: guidesList.width
                height: 25
                Text {
                    text: title
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: guidesList.currentIndex = index
                    onDoubleClicked: {
                        model.modelData.currentSlide = -1;
                        goToPage({'name': 'INFO', 'modelData': model.modelData});
                    }
                }
            }
        }

        ListView {
            id: guidesList
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height - frameHMargin
            width: parent.width - frameVMargin
            focus: true
            model: guidesModel
            highlight: Rectangle { color: "lightsteelblue"; radius: 5 }
            delegate: guidesDelegate
            Keys.onReturnPressed: {
                currentItem.modelData.currentSlide = -1;
                goToPage({'name': 'INFO', 'modelData': currentItem.modelData});
            }
        }
    }
}
